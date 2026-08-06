# git_diff_hunks — ANTS-3377 conformance

Pure-parser feature test for `GitWrap::parseDiffHunks` (src/gitwrap.cpp),
the git-free core of the `git_state op:diff` hunk-header mode. Drives the
helper against literal unified-diff fixtures — no git, no QProcess, no
repo — so every case runs unconditionally (unlike the sibling
`gitwrap_stdout_cap`, which needs `git` on PATH).

## Why this exists (ANTS-3377)

The roadmap asked for a read-only way to see **which line ranges changed**
in each file, for splitting a messy working tree into separate clean
commits. `git_state op:diff` already existed but returned only `--numstat`
line *counts* per file — not the `@@` hunk boundaries a commit split needs.
Rather than add a parallel `git_diff` verb that would clone
`op:diff`'s root resolution / path+range validation / GitWrap error
mapping, the capability is folded into `op:diff` via a `hunks` flag, backed
by this pure parser (reuse-over-rewrite).

## Invariants covered

- **INV-1 (hunk parse)** — a modified file yields one `DiffHunk` per `@@`
  block with `oldStart/oldCount/newStart/newCount` parsed from the header
  and `header` holding the full `@@ … @@ <section>` line. Path is taken
  from the `+++ b/<path>` line.
- **INV-2 (omitted count → 1)** — `@@ -30 +31,2 @@` parses `oldCount == 1`
  (git omits a count of 1).
- **INV-3 (added file)** — `--- /dev/null` + `+++ b/<path>`: path from the
  `+++` side; pre-image range is `0,0`.
- **INV-4 (deleted file)** — `--- a/<path>` + `+++ /dev/null`: path from the
  `---` side.
- **INV-5 (body + ambiguity)** — with `includeLines`, each hunk carries its
  raw body lines (leading ` `/`+`/`-` marker kept). A body line that itself
  begins `+++ ` (an added line whose text starts with `++`) is NOT mistaken
  for a file header — the `--- `/`+++ ` path parse is gated on "no hunk
  opened yet", resolving the classic unified-diff ambiguity.
- **INV-6 (hunk-less omitted)** — a file with a `diff --git` header but no
  `@@` (pure rename / mode change) is omitted; numstat mode already reports
  those.
- **INV-7 (empty)** — empty / hunk-less input returns an empty vector, no
  crash.

## MCP layer (not asserted here — behavioural, manual)

`runDiffOp` (the remotecontrol TUs) maps the parsed structs to the
`op:diff` envelope when `hunks=true`:
`{files:[{path, hunks:[{header, old_start, old_count, new_start,
new_count, lines?}]}], totals:{files}}`, with `lines` present only when
`include_lines=true`. `staged=true` diffs the index vs HEAD
(`git diff --cached`) and is mutually exclusive with `range` (refused
`bad_args`). `context` (0..10, default 3) sets `--unified`. The envelope
still flags `worktree` / `staged` / `range` so the caller knows which diff
it received; `truncated` flags a diff that hit the 1 MiB stdout cap. This
wiring is exercised via manual relaunch + a Claude-side `git_state
{op:"diff", hunks:true}` call (same convention as the `mcp_git_state`
source-grep harness).

## Out of scope

- The `--numstat` default path is unchanged and byte-identical for existing
  callers — covered by manual behavioural verification, not re-asserted.
- Binary-file hunks: `git diff` emits `Binary files … differ` with no `@@`,
  so such files are omitted (same as pure renames).
