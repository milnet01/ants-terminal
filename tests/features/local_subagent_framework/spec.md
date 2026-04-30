# Local-subagent framework — ants-helper CLI v1 (ANTS-1116)

User request 2026-04-30: ship a stateless CLI helper Claude Code can
invoke instead of running shell scripts and parsing their stdout
manually. v1 surface: a single subcommand wrapping
`packaging/check-version-drift.sh`, behind a CMake option that
defaults OFF. See `docs/specs/ANTS-1116.md` for the full contract.

## Surface

- `AntsHelper::driftCheck(request, repoRoot, *exitCode)` — pure
  handler returning the unified `{ok, ...}` envelope.
- `ants-helper` binary — thin argv/stdin dispatcher that calls the
  handler and prints the JSON to stdout.
- Build is gated by the `ANTS_ENABLE_HELPER_CLI` CMake option
  (default OFF).

## Invariants

- **INV-1** Building with `-DANTS_ENABLE_HELPER_CLI=OFF` (the
  default) produces no `ants-helper` target; `-DANTS_ENABLE_HELPER_CLI=ON`
  adds exactly one new target without modifying the GUI binary.
- **INV-2** `driftCheck` against a clean tree (drift script
  exits 0) returns `{"ok": true, "data": {"clean": true}}` and
  sets `*exitCode = 0`.
- **INV-3** `driftCheck` against a tree with a fake drift script
  that exits non-zero with synthetic `file:line: msg` violations
  returns `{"ok": true, "data": {"clean": false, "violations":
  [...], "raw": "...", "exit_code": N}}` and sets `*exitCode = 3`.
  Drift detection stays in `ok: true` (handler ran fine; result is
  "not clean" — distinct from a handler error).
- **INV-4** `driftCheck` against a directory with no
  `packaging/check-version-drift.sh` returns
  `{"ok": false, "error": "...", "code": "missing_script"}` and
  sets `*exitCode = 1`.
- **INV-5** `driftCheck` against a non-existent directory returns
  `{"ok": false, "error": "...", "code": "missing_repo_root"}`
  and sets `*exitCode = 1`.
- **INV-6** Drift-detected payload includes both `raw` (the
  script's full stdout, untruncated string) and `exit_code`
  (number — the script's exit code) — pinning ANTS-1116 INV-3a.
- **INV-7** `jsonToCompactString` produces single-line UTF-8 JSON
  (no embedded literal newlines outside string values), so the
  CLI dispatcher's stdout is one JSON object per call.
- **INV-8** Source-grep INV-1 corollary: `CMakeLists.txt` declares
  `option(ANTS_ENABLE_HELPER_CLI ... OFF)` and gates an
  `add_executable(ants-helper ...)` block on that option.
