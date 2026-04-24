# Feature: Claude Code `UserPromptSubmit` git-context hook

## Problem

When the user runs Claude Code inside Ants, every question that needs
"what does `git status` say here?" context makes Claude spend tokens
on a `Bash(git status)` tool call:

- The request includes tool-use JSON envelope + command bytes.
- The response includes tool-result JSON + the command's stdout.
- The model pays attention tokens to decide that `git status` is what
  it needs, then to parse the porcelain output.

Over a long coding session this adds up: a ~150-byte porcelain output
plus tool-call JSON envelope plus model reasoning costs on the order
of 400–600 tokens per invocation, repeated dozens of times. The user
wants a mechanism that hands Claude the current git state *up front*
on every prompt, so the model has it without asking.

User ask 2026-04-24: *"Ants Terminal keeps track of the git status.
How can we give Claude Code access to this so that Claude Code
doesn't need to use up tokens checking the git status."*
Follow-up: *"Can this hook be available to all projects?"*

## External anchors

- [Claude Code hooks reference](https://docs.claude.com/en/docs/claude-code/hooks)
  — `UserPromptSubmit` hook fires before each user message is sent to
  the model; hook stdout is prepended to the user's prompt as
  additional context.
- [Claude Code `CLAUDE_PROJECT_DIR` env var](https://docs.claude.com/en/docs/claude-code/hooks#environment-variables-available-in-hooks)
  — Claude Code exports this automatically for every hook invocation,
  pointing at the session's root working directory. The script relies
  on it so the hook works regardless of the process's own cwd.
- [Claude Code settings-file precedence](https://docs.claude.com/en/docs/claude-code/settings)
  — `~/.claude/settings.json` is user-scope (applies to every project
  the user opens), `.claude/settings.json` in a repo is project-scope
  (committed), `.claude/settings.local.json` is project-scope-local
  (gitignored). This feature uses user-scope so the hook applies
  **globally across every project** (the user's explicit ask).
- [Git status porcelain v1 format](https://git-scm.com/docs/git-status#_porcelain_format_version_1)
  — stable, parser-friendly output; our script matches on the first
  two columns (`XY`) to count staged / unstaged / untracked.

## Scope

**In scope:**

- A shell helper script shipped + installed by Ants that, when
  invoked as a `UserPromptSubmit` hook, prints a compact git-status
  block to stdout. Claude Code injects that stdout into the user's
  prompt as additional context.
- An Ants Settings-dialog installer that merges the hook entry into
  `~/.claude/settings.json` under `hooks.UserPromptSubmit` — global
  across every project.
- Graceful no-ops: not a git repo, git binary missing, or any
  subcommand fails → exit 0 with empty stdout. A hook that errors on
  non-repo dirs would annoy the user on every non-repo prompt.
- Existing `hooks.UserPromptSubmit` entries (user-added or per-project)
  are preserved; our entry is appended only if not already present.
- Existing `~/.config/ants-terminal/hooks/claude-forward.sh`
  (status-bar hook) is untouched. This feature lives in a separate
  script and a separate hook event, so its presence/absence is
  independent.

**Out of scope:**

- Caching inside Ants. `setGitStatusProvider` in `claudeintegration.cpp`
  already has a synchronous cache, but it runs three `git` subprocesses
  with 2 s timeouts — not suitable for the 2 s status-bar poll. Adding
  a periodic cache would introduce a main-thread stall (the 2026-04-20
  user report already flagged stalls from the 2 s timer). Running git
  inside the hook is cheap: <100 ms on any normal repo, off Ants'
  main thread entirely.
- Recent-commands / cwd-history injection. Future `UserPromptSubmit`
  extension; the hook design accommodates adding more `<…>` blocks
  later without breaking existing ones.
- Uninstall UI. The installer is idempotent (running it twice is a
  no-op). Removing the hook is a manual edit of `~/.claude/settings.json`
  — acceptable for a first slice; matches the existing status-bar
  hook installer's behaviour.
- Projects where `.claude/settings.json` **disables** global hooks
  via the `hooks.UserPromptSubmit` key with our command removed.
  Claude Code's precedence rules say project-scope wins; we don't
  override that.

## Contract

### Invariant 1 — Helper script exists at a well-known path

Location: `~/.config/ants-terminal/hooks/claude-git-context.sh`.
Mode 0755. Idempotent — running the installer twice leaves one
script, same content.

Path is exposed via `ConfigPaths::antsClaudeGitContextScript()` (new
function in `src/configpaths.h`, keeping path knowledge in one file
for the same reason the existing `antsClaudeForwardScript()` exists).

### Invariant 2 — Helper script is a no-op outside a git repo

The script MUST:

- Exit 0 with empty stdout when `CLAUDE_PROJECT_DIR` is unset AND
  `$PWD` is not inside a git work tree.
- Exit 0 with empty stdout when `git` is not on `$PATH`.
- Exit 0 with empty stdout when `git rev-parse --is-inside-work-tree`
  returns non-zero.

Silent no-op is the only acceptable behaviour. A hook that prints
error chatter pollutes every prompt in projects that don't need
this feature.

### Invariant 3 — In a git repo, the script emits exactly one `<git-context>` block

Format:

```
<git-context>
Branch: <branch-name> (ahead <N>, behind <M>)
Upstream: <upstream> | (none)
Staged: <N> file(s)
Unstaged: <N> file(s)
Untracked: <N> file(s)
</git-context>
```

- `<branch-name>` is `git rev-parse --abbrev-ref HEAD`. If detached,
  the first 7 chars of the SHA.
- `(ahead N, behind M)` is omitted entirely when there is no
  upstream, not printed as `(ahead 0, behind 0)` — that distinction
  matters (an unpushed new branch vs a sync'd tracking branch).
- `Upstream: <upstream>` shows the short-form upstream
  (`origin/main`) or the literal `(none)` when no upstream.
- Line counts use `git status --porcelain` with the documented XY
  two-column prefix:
  - Staged (first column): `M | A | D | R | C` (excluding `?`).
  - Unstaged (second column): `M | D | A | R | C` (excluding space
    and `?`).
  - Untracked: both columns are `?` (i.e. the `??` prefix).
  - A line with both a staged AND unstaged change counts in **both**
    buckets — the user wants to know "this file has staged changes
    AND further unstaged edits", the dual-count signals that.
- Pluralisation: the word is `file(s)` on every line regardless of
  count. Pragmatic — avoids a `file` / `files` branch for one
  character of elegance.

The `<git-context>` open/close tags are fixed literals so a future
reader grepping transcripts or the model itself can locate the
block deterministically.

### Invariant 4 — Zero-count lines are still emitted

If `Staged: 0 file(s)`, the line is still printed. Rationale: the
model sees a predictable block structure on every prompt; a missing
line means "something failed", not "there's nothing to say." A
predictable absence of surprises is the contract.

### Invariant 5 — Installer writes `~/.claude/settings.json` with UserPromptSubmit entry

New method `SettingsDialog::installClaudeGitContextHook()` (or a
flag on the existing `installClaudeHooks()`; see Invariant 9 for
the UI decision). The method:

- Creates `~/.config/ants-terminal/hooks/` if missing.
- Writes `claude-git-context.sh` (Invariant 1 path), 0755.
- Reads `~/.claude/settings.json` via the same
  `QFile` + `QJsonDocument::fromJson` + parse-error-rotate-aside
  pattern the existing `installClaudeHooks()` uses (matches the
  0.7.12 /indie-review cross-cutting fix; refuses to overwrite a
  file it can't parse).
- Locates `hooks.UserPromptSubmit` (creating an empty array if
  missing).
- Appends exactly one entry referencing the script, **only if** our
  script path isn't already referenced somewhere inside that array.
  Existing user hooks (pre-commit linters, custom context injectors,
  etc.) are preserved verbatim.
- Writes the merged settings back via `QSaveFile::commit()` for
  atomic replacement.

The entry shape matches Claude Code's documented hook schema:

```json
{
  "matcher": "",
  "hooks": [
    { "type": "command", "command": "/home/user/.config/ants-terminal/hooks/claude-git-context.sh", "timeout": 2 }
  ]
}
```

The `matcher` field is empty (Claude applies to every user prompt);
the `timeout` is 2 seconds (matches the other Ants-installed hooks
and is well above the <100 ms typical script runtime).

### Invariant 6 — Installer is idempotent

Running `installClaudeGitContextHook()` twice MUST:

- Leave one copy of the script at the canonical path.
- Leave exactly one entry in `hooks.UserPromptSubmit` that references
  our script (no duplicates, no stale sibling entries from older
  script-path conventions).
- Preserve every non-Ants hook entry that was in the array before
  the first call.

### Invariant 7 — Installer is global-scope only, never project-scope

The installer writes `~/.claude/settings.json` (`ConfigPaths::claudeSettingsJson()`).
It MUST NOT touch any project-directory `.claude/settings.json`. The
user's explicit ask is "available to all projects" — project-scope
installs would be narrower, and would also mean Ants writes files
into arbitrary project directories the user may not have asked us
to touch. This is a hard boundary.

### Invariant 8 — Hook respects `CLAUDE_PROJECT_DIR` over `$PWD`

Claude Code sets `CLAUDE_PROJECT_DIR` in the hook's environment.
When set, the script MUST `cd "$CLAUDE_PROJECT_DIR"` before running
git. When unset, it falls back to `$PWD`. Rationale: hooks are
spawned by Claude Code with Claude's cwd, which may not match the
project root if the user has `cd`'d elsewhere in the same session.
`CLAUDE_PROJECT_DIR` is the canonical "what repo is this session
rooted in" signal.

### Invariant 9 — UI: dedicated checkbox/button, separate from status-bar hooks

The Settings dialog's Claude Code section gains a distinct
control for this hook — NOT bundled behind the existing
"Install hooks" button. Rationale:

- The existing hook button installs five event hooks
  (SessionStart, PreToolUse, PostToolUse, Stop, PreCompact) that
  forward events to Ants for real-time status-bar updates.
- This new hook does the **opposite** data flow: Claude pulls data
  *from* git, not Ants pushing data to Claude.
- Users who want one feature and not the other should be able to
  opt in independently. Bundling them couples two unrelated
  user benefits.

Minimum viable UI: a second button "Install git-context hook" with
a status label below ("✓ Installed" / "Not installed"), mirroring
the existing pattern at `settingsdialog.cpp:920-945`.

### Invariant 10 — Pre-existing `UserPromptSubmit` entries survive

A user who has (before running our installer) a custom
`UserPromptSubmit` hook in `~/.claude/settings.json` — e.g. a
ripgrep-cheatsheet injector, a TODO lister, a team-policy reminder —
MUST retain that hook after our installer runs. Our entry is
appended to the existing array; we do not clobber, re-order, or
dedupe sibling entries.

This is the same preservation invariant the existing hook
installer carries (explicitly called out at `settingsdialog.cpp:1062`:
*"keeps user-added custom hooks on the same event intact."*).

## Regression history

- **Introduced:** n/a — this is a new feature.
- **User ask:** 2026-04-24, during a session on the AI-redaction work.
  User observed Claude Code rerunning `git status` repeatedly and
  asked if Ants could short-circuit it.
- **Prior-art references consulted:**
  - Claude Code hooks reference (link above).
  - Existing Ants hook installer (`settingsdialog.cpp:947-1098`) —
    architecture, JSON-merge-preserving pattern, 0.7.12 parse-error
    refusal.
  - `ClaudeAllowlist` `saveSettings` (same preservation pattern for
    the `permissions.allow` key).

## What fails without the feature

- User asks "what's the state of this repo?" → Claude runs
  `Bash(git status --porcelain)` → ~500 tokens burned per turn →
  repeat on next question.
- User asks "commit what I've changed" → Claude runs `git status` to
  find what to stage → same cost.
- User asks "why is this file different from main?" → Claude runs
  `git status` to see the change state → same cost.

With the feature: every prompt has the status baked into context.
Claude doesn't need to ask.

## Test strategy

**Unit-level script test** (`tests/features/claude_git_context_hook/test_script.sh`):

1. **Not-a-repo control:** `cd /tmp; $script` → exit 0, empty stdout.
2. **Git-missing control:** `PATH=/nonexistent $script` → exit 0,
   empty stdout.
3. **Clean repo:** git-init a tmpdir, `cd` in, `$script` → emits one
   `<git-context>` block with `Branch: main`, zero counts, no
   `(ahead …, behind …)` clause.
4. **Dirty repo:** add one untracked file, stage one modified file,
   leave one unstaged modification → counts are `Staged: 1`,
   `Unstaged: 1`, `Untracked: 1`.
5. **`CLAUDE_PROJECT_DIR` honored:** script run with `PWD=/tmp`
   and `CLAUDE_PROJECT_DIR=<tmp-repo>` emits the repo's status,
   not `/tmp`'s.

**Source-grep test** (`tests/features/claude_git_context_hook/test_installer_grep.cpp`):

Asserts, against `src/settingsdialog.cpp`:

- INV-5a: `installClaudeGitContextHook()` (or equivalent) exists.
- INV-5b: It writes the script path from
  `ConfigPaths::antsClaudeGitContextScript()`.
- INV-5c: It targets `UserPromptSubmit` in the merged JSON.
- INV-5d: The JSON-merge path uses the same parse-error-refuse
  pattern as `installClaudeHooks()` (no clobber on corrupt
  settings).
- INV-5e: `hooks.UserPromptSubmit` existing-array handling uses the
  same "append only if our path not already present" loop as the
  five-event installer.
- INV-10: `"UserPromptSubmit"` string appears in the new installer
  AND the EXISTING installer's skip-list comment / documentation
  still mentions preservation (regression guard: if someone
  refactors and changes the preservation behaviour, both halves
  fail together).

**Verification — runtime behaviour pre-fix vs post-fix:**

Against pre-fix source: script file doesn't exist, the cpp doesn't
contain `installClaudeGitContextHook`, the grep invariants fail.
Against post-fix: all invariants pass. The shell test only runs
post-fix (needs the script to exist).

## Token-economy estimate

Per user prompt with the hook active:

- Output added to prompt: ~120 bytes = ~40 tokens.
- Claude saves: one `Bash(git status)` round-trip when relevant
  (~400-600 tokens including tool-use JSON + stdout parsing).

Break-even: the hook pays for itself after one avoided `git status`
call over any 10-prompt session. In typical dev sessions the model
reruns `git status` multiple times per turn (before and after
edits); the savings compound.

## Security / UX considerations

- **Trust boundary.** The script runs inside Claude Code's process
  tree, same UID as the user, same working directory as the
  session. No new privilege, no new network exposure.
- **Noise.** Empty-output no-ops mean projects that aren't git
  repos (docs sites, experimental scratch dirs) see zero behaviour
  change.
- **User awareness.** The `<git-context>` tags are visible to the
  user in the Claude Code transcript (Claude echoes the prompt
  including injected context). They can see *what* Ants is
  injecting and *when*. No covert modification.
- **Token budget.** The injected block is small enough that even a
  user on a tight context-window budget pays a negligible cost,
  and the savings on avoided tool calls dominate within a few
  turns.
