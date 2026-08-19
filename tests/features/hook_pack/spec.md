# hook_pack — ANTS-1252 conformance

Source spec: [docs/specs/ANTS-1252.md](../../../docs/specs/ANTS-1252.md).

Shell-driven harness (no C++ link, no QProcess wrapper) following the
`tests/audit_self_test.sh` pattern. Exits 0 when every assertion
passes, non-zero (with FAIL lines on stderr) otherwise.

## What this test asserts

1. **INV-1 (no-op exit 0).** Every script in `hooks/ants-*.sh`
   exits 0 when fed `printf '{}\n'` on stdin from inside the project
   root.
2. **INV-3 (preamble cap).** `ants-session-preamble.sh` stdout
   ≤ 500 bytes against `{}` stdin.
3. **INV-4 (bash-veto reason cap).** Bash veto's `reason` field
   ≤ 200 bytes for each pattern that triggers a block.
4. **INV-7 (jq-only payload parse).** `ants-bash-veto.sh` and
   `ants-read-roadmap-veto.sh` source contain `jq -r` and do NOT
   contain raw-regex against the JSON input (no `grep -P` / `sed`
   over the unparsed payload).
5. **INV-10 (silent side-effect hooks).** `ants-drift-check.sh` and
   `ants-precompact-snapshot.sh` emit zero stdout bytes against
   `{}` stdin.
6. **INV-12 (no leakable bypass token).** For each pattern that
   fires a block, the emitted block-reason JSON's `.reason` MUST
   NOT contain the literal `# ants-bypass` token. The token DOES
   appear in `ants-bash-veto.sh` source (inside the `case` glob
   that *enables* the bypass) — INV-12's concern is leakage into
   the model's input, i.e. the reason string. The override
   mechanism is documented in `hooks/README.md` only.
7. **Bash-veto block hits.** Two positive cases that MUST produce a
   `decision:"block"` JSON envelope: `git status`, `cat ROADMAP.md | grep`.
   One negative: `ls -la` produces no envelope (empty stdout). (ANTS-2141
   moved `grep -r … src/` out of the block class into the soft-warn class —
   see the ANTS-2141 section below.)
8. **Bash-veto bypass.** A trailing `# ants-bypass` on the proposed
   command suppresses the veto (empty stdout).
9. **Read-roadmap veto.** Block fires for full-file reads of the
   project's `ROADMAP.md`; suppressed when `offset` or `limit` is
   present, and for unrelated file paths.
10. **INV-9 silence outside project.** Each hook, run from a
    directory with NO `.ants-project` ancestor, exits 0 with empty
    stdout.
11. **INV-2 sessionId regex.** `_common.sh::ants_validate_session_id`
    accepts `abc123_-`, rejects `../foo`, `a/b`, the empty string,
    and a 65-char overflow.
12. **install-hooks.sh round-trip (also covers INV-5).** `--dry-run`
    against a tmp target produces JSON the post-install validator
    parses; live install + uninstall round-trips back to removing
    the sentinel key; a re-run on top of an existing install is
    idempotent (INV-1).
13. **INV-5 (symlink abort).** `tools/install-hooks.sh` against a
    target that `lstat`s as a symlink exits non-zero with a
    descriptive message — verified inside the install-hooks
    round-trip block.

## ANTS-2141 — grep/find soft-warn (appended; existing 1-13 unchanged)

Source spec: [docs/specs/ANTS-2141.md](../../../docs/specs/ANTS-2141.md).
These run only when `jq` is present (skipped with a `[skip]` line otherwise).

- **Warn class (INV-1/3/5/12).** `grep -rn … src/`, pathless `grep -rn`, `rg`,
  `git grep`, `find src -name`, and `grep -rn x src/ | head` each emit a
  PreToolUse `additionalContext` envelope with **no** `permissionDecision` and
  **no** top-level `decision`; the field is ≤ 400 B (measured 237 B).
- **No-warn class (INV-4/5).** Empty stdout for: piped grep
  (`cmake … | grep`), single-file `grep foo file.txt`, exempt `/var/log` /
  `app.log` / `--help` / `# ants-bypass`, and non-eligible `find build -name x`
  / `find . -name x -delete`.
- **Predicate (INV-6).** `_common.sh::ants_is_source_search` is sourced and
  table-tested in-process (it `return`s, never `exit`s).
- **Throttle (INV-7).** With `ANTS_GREP_NUDGE_THROTTLE_SEC=2` + a fixed
  `ANTS_GREP_NUDGE_KEY`: first fire warns, an immediate second is suppressed
  (empty), and a fire after the window (`touch -d` backdate) warns again.
- **Counter (INV-8).** Every eligible match appends one `count.jsonl` line
  (incl. `warned:false` for the suppressed fire); seeding 600 lines then firing
  truncates to exactly 251.
- **Fail-open (INV-9).** With the cache path forced unwritable (a regular file
  where the dir should be), the hook still emits the warn and exits 0.
- **Outside project (INV-11).** Run from a non-ants dir, no `count.jsonl` /
  stamp is created.

## ANTS-2023 — cat/head/tail/bat read-dump soft-warn (appended; 1-13 + ANTS-2141 unchanged)

Source spec: [docs/specs/ANTS-2023.md](../../../docs/specs/ANTS-2023.md).
These run only when `jq` is present (skipped with a `[skip]` line otherwise).

- **Warn class (INV-1/3).** `cat src/…`, `cat include/…`, `head -50 …cpp`,
  `tail -100 tests/…cpp`, and `bat src/…lua` each emit a PreToolUse
  `additionalContext` envelope with **no** `permissionDecision` and **no**
  top-level `decision`; the field is ≤ 400 B (measured 225 B).
- **No-warn class (INV-4).** Empty stdout for EXEMPT (`build-fast/…`, `/etc/…`,
  `.log`-suffix, `--help`, `# ants-bypass`) and non-eligible (markdown
  `README.md`, `notes.txt`, redirect `> bar`, heredoc `<< EOF`, piped
  `… | grep`, separated option-arg `head -n 50 …` — blind spot d).
- **Predicate + disjointness (INV-5).** `_common.sh::ants_is_source_read` is
  sourced and table-tested in-process (it `return`s, never `exit`s); a
  cross-check asserts no command is classified by **both**
  `ants_is_source_read` and `ants_is_source_search`.
- **Shared throttle (INV-6).** Reusing ANTS-2141's per-`$PPID` throttle: a `grep`
  warn immediately followed by a `cat src/…` emits empty (suppressed by the
  shared window); after the window the read warns. `count.jsonl` holds exactly 3
  lines (one `warned:false`), proving the read branch records on both paths.

### Not covered (deliberately)

- **INV-6** (sentinel-key fence) — partially exercised by the
  install + uninstall round-trip in assertion 12 (the sentinel key
  is set on install, removed on uninstall). A dedicated assert that
  rejects text-fence comments is omitted as the install path never
  emits comments.
- **INV-8** (jq-validate-before-rename) — would need a fixture that
  forces tmpfile corruption before rename; defer to manual smoke
  test until a regression motivates it.
- **INV-11** (drift-check `flock` PID-file) — would need a
  concurrent-fork race; defer to manual smoke test for the same
  reason.

## ANTS-2169 Part 2 — git veto routes a diff to the verb that can answer it

Four assertions, appended; everything above is unchanged.

| # | Asserts |
|---|---------|
| G1 | A `--stat` diff still blocks, and its reason names `git_state`. |
| G2 | That reason does NOT prescribe `get_git_status`. |
| G3 | The reason obeys INV-4 (1..200 B) on the new branch too. |
| G4 | The status/log branch still routes to `get_git_status`. |

G2 is the one with teeth. G1 alone passes if a future edit adds
`git_state` to the old shared string while leaving the wrong verb in
front of it, which is the shape the original defect had: the resolution
note for ANTS-2169 Part 2 correctly identified `git_state op:diff` and
the message still sent people to `get_git_status`. Naming the right verb
somewhere is not the same as routing to it.

G4 exists because the fix SPLIT one case branch into two. The obvious
way to get that wrong is to take `get_git_status` with the diff, leaving
a plain `git status` routed to a diff verb — a regression the diff-side
assertions cannot see.

**Run red before trusting these.** Verified 2026-08-19 against the
pre-fix hook: G1 and G2 failed with the wrong reason quoted, while G4
passed on both versions — which is what proves the red run was measuring
the routing rather than a broken harness.

**Would break this:** asserting only that the diff blocks (the pre-fix
hook blocks too, just wrongly); matching the reason on `diff` rather than
on the verb name (the wrong string contains the word); dropping G4 after
the split looks settled.

## Why shell, not C++

The hooks are bash scripts; the install path is bash + jq; no
production code links them. A C++ harness would only proxy back to
`bash <hook>` anyway. Shell is the natural surface — same call as
the real Claude Code hook runtime.

## Project-root caveat

INV-9 cases run inside a `(cd "$outside" && …)` subshell so the
parent harness shell stays in `$REPO_ROOT` throughout — there is no
explicit cd-back step, the subshell pattern is what protects later
assertions. The outside fixture is `mktemp -d -t
ants-hook-pack-out.XXXXXX`; the install-hooks round-trip uses a
separate `ants-hook-install.XXXXXX` template. Either prefix means
a stale fixture from a crashed run is isolated from the next.

## Skip behaviour

If `jq` is not on PATH, the harness skips: INV-4 + bash-veto
behavioural cases, INV-12, read-roadmap-veto behavioural cases, and
the install-hooks round-trip. INV-1, INV-3, INV-7 (source-grep),
INV-9, INV-10, and INV-2 still run. Every skip is announced with a
`[skip]` line — never silent. CI runners without jq fail the
assertion only if jq's absence would mask a real bug (none
currently).
