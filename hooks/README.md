# Ants Terminal Claude Code hook pack

The scripts here are installed user-globally into `~/.claude/settings.json` by
[`tools/install-hooks.sh`](../tools/install-hooks.sh) and gated per-project by an
`.ants-project` marker (so they fire only inside an Ants repo, costing a
sub-millisecond `stat` and zero tokens elsewhere).

Design contract: [`docs/specs/ANTS-1252.md`](../docs/specs/ANTS-1252.md)
(the pack) and [`docs/specs/ANTS-2141.md`](../docs/specs/ANTS-2141.md)
(the grep/find soft-warn). Conformance test:
[`tests/features/hook_pack/`](../tests/features/hook_pack/).

| Script | Event | What it does |
|---|---|---|
| `ants-session-preamble.sh` | SessionStart | One compact orientation block (≤ 500 B). |
| `ants-bash-veto.sh` | PreToolUse(Bash) | Soft-warn + block routing (below). |
| `ants-read-roadmap-veto.sh` | PreToolUse(Read) | Blocks a full Read of a large `ROADMAP.md` → `roadmap_query`. |
| `ants-drift-check.sh` | Stop | Side-effect-only drift check. |
| `ants-precompact-snapshot.sh` | PreCompact | Saves a resume snapshot. |
| `_common.sh` | — | Shared helpers (sourced, not executed). |

## `ants-bash-veto.sh` — two routing classes

**Soft-warn (ANTS-2141, non-blocking).** A *raw source search* — `grep -r`/`-R`,
`rg`/`ag`/`ack` (recursive by default), `git grep`, or a `find` under
`./`/`src`/`tests`/`include` with a `-name`/`-path` primary — gets a PreToolUse
`additionalContext` reminder to prefer the indexed MCP verbs
(`mcp__ants__workspace_search` / `find_definition` / `find_sources`). **The
command still runs** — nothing is blocked. It is **not** warned when:

- piped from another command (`cmake … | grep error`),
- a single-file / non-recursive grep (`grep foo file.txt`),
- over a non-source location (`/tmp`, `/var`, `build/`, `node_modules`, a
  `*.log` file, an absolute path outside the repo …),
- a destructive/exec `find` (`-delete`, `-exec`, …),
- carrying `--help`/`--version`,
- or carrying the `# ants-bypass` override (below).

**Soft-warn (ANTS-2023, non-blocking).** A *source read-dump* — `cat`/`head`/
`tail`/`bat` of a **code file** (a path under `src`/`tests`/`include`, or one
ending `.cpp`/`.cc`/`.cxx`/`.c`/`.h`/`.hpp`/`.hh`/`.lua`) — gets a PreToolUse
reminder to prefer `mcp__ants__file_outline` (symbols/structure) or
`mcp__ants__read_region` (a line range) instead of streaming the whole file back.
**The command still runs.** It is **not** warned when piped (`cat x | grep` —
that's processing), redirected/heredoc (`> bar`, `<< EOF` — writing/feeding),
over a non-source location or `.log`, for markdown/config/text (`cat README.md` —
out of scope; `ROADMAP.md` has its own routing), or carrying `--help` /
`# ants-bypass`. It shares the same anti-nag throttle as the search nudge below
(at most one prefer-MCP reminder per window, search *or* read).

**Block (precise vetoes).** `git status` / `git log --oneline` /
`git diff --stat` → `get_git_status`; `cat ROADMAP.md | grep` → `roadmap_query`.
These emit `{"decision":"block","reason":"…"}` (reason ≤ 200 B).

### The `# ants-bypass` override

Append `# ants-bypass` to any Bash command to suppress **all** veto/warn
behaviour for that one command:

```bash
grep -r foo src/ # ants-bypass
```

The marker is stripped before any pattern match and never echoed back to the
model. It is a **UX affordance, not a security boundary** (a shell hook cannot
defend against prompt-injected commands; the security boundary is the UID gate +
UDS perms, ANTS-1132).

## Throttle + observability (ANTS-2141)

To avoid nagging (and spending the tokens the warn exists to save), the nudge is
emitted at most once per `ANTS_GREP_NUDGE_THROTTLE_SEC` (default 600) per Claude
process. Tuning env vars:

- `ANTS_GREP_NUDGE_THROTTLE_SEC` — window in seconds (default 600).
- `ANTS_GREP_NUDGE_KEY` — throttle key (default `$PPID`); a seam for a future
  session-id key.

Every eligible search — warned or throttle-suppressed — is tallied to
`~/.cache/ants-terminal/grep-nudge/count.jsonl` (bounded to ≤ 500 lines). Compare
that "grep side" against the "index side" (`token_usage`'s per-verb `n_calls` for
`workspace_search` / `find_definition` / `find_sources`) with
[`tools/grep-vs-index.sh`](../tools/grep-vs-index.sh) to see whether the session
is greping or querying the index.

A broken cache never silences the warn — the throttle **fails open** (emits).
