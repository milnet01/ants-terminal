# workspace_search timeout fallback — feature contract

Test contract for **ANTS-6245** (`docs/specs/ANTS-6245-search-timeout-git-fallback.md`).
The invariants, their rationale, scope and regression history live in that
spec, § 5 onward. This file names the cases and does not restate them.

Each feature case builds a real git repository in a `QTemporaryDir`, then calls
`RemoteControl::cmdWorkspaceSearch` twice with the same request: plain, and
with `setSearchTimeoutForced(true)`. It compares the match rows as a set of
`(file, line, text)`. Every fixture holds several tracked files without the
pattern, so the skip list is non-empty.

## Invariants

- **INV-1** — A completed fallback returns the plain scan's rows and carries `timeout_fallback:"git_index"`. Case `sameRowsAsPlainScan`.
- **INV-2** — A match that needs Unicode case folding is found. Case `unicodeFoldKept`.
- **INV-3** — A tracked UTF-16 file with a byte-order mark is found. Case `utf16Kept`.
- **INV-4** — A match in a modified, assume-unchanged, skip-worktree, untracked or unmerged file is found. Case `dirtyStatesKept`.
- **INV-5** — A match in a file with an `ident` attribute is found. Case `attributedKept`.
- **INV-6** — The fallback reports no row the plain scan lacks. Case `noExtraRows`.
- **INV-7** — Paths holding glob metacharacters round-trip. Cases `globEscape` (unit) and `oddNamesRoundTrip`.
- **INV-8** — Each skip condition replies as today with its `timeout_fallback_skipped` value. Case `skippedReasons`.
- **INV-9** — A project root below the git top level, and `lane`, keep INV-1. Case `subdirRootAndLane`.
- **INV-10** — First scan plus fallback stay within the maximum budget. Case `fallbackBudget` (unit).
- **INV-11** — The pattern reaches git only as file content, shell-less. Case `patternNotInArgv`.

## Notes

- Each invariant's test surface is its paired file: `test_workspace_search_timeout_fallback.cpp` and `test_searchprefilter.cpp`.
- `unicodeFoldKept` uses `xyztest` where the spec says `test`: the needle of plain `test` is `te` (two bytes), which the spec's own § 4.1 sends to `short_needle`, so no fallback would run.
