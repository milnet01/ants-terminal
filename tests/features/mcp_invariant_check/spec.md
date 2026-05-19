# mcp_invariant_check — ANTS-1308

Locks the wiring contract for the `invariant_check` MCP tool — the
"which specs reference this file, and what invariants do they
declare" pre-edit surface that scans `docs/specs/*.md` for
substring mentions of the input `files[]`.

## Invariants

| # | Statement |
|---|-----------|
| 1 | `cmdInvariantCheck(const QJsonObject &req)` declared public on `RemoteControl` in `src/remotecontrol.h`. |
| 2 | `cmdInvariantCheck` defined in `src/remotecontrol.cpp` and carries an `ANTS-1308` anchor comment in or above the function body. |
| 3 | The body refuses with `code:"bad_files"` when the `files` argument is missing, empty, or normalises to an empty list. |
| 4 | The body iterates `docs/specs/ANTS-*.md` via `QDir` (the directory listing surface, not a hardcoded file list). |
| 5 | The body delegates parsing to the shared `parseSpecBody` helper (same helper that backs `cmdSpecQuery`, so the parser is single-sourced). |
| 6 | `MainWindow::setupClaudeMcpProviders` (`src/mainwindow.cpp`) registers `"invariant_check"` via `registerToolProvider` and delegates to `m_remoteControl->cmdInvariantCheck`. Falls back to `kRcUnavailable` when `m_remoteControl` is null. |
| 7 | The `tools/list` block in `src/claudeintegration.cpp` registers an `"invariant_check"` entry. Schema declares `properties.files` (required, array of string, minItems 1) and `properties.caller_cwd` (required). |
| 8 | `callerCwdContractFor` in `src/claudeintegration.cpp` classifies `"invariant_check"` as `Required` (explicit branch). |

## Acceptance

Exit 0 = all 8 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. Uses the existing
`SRC_CLAUDE_INTEGRATION_CPP_PATH`, `SRC_RC_HEADER`,
`SRC_REMOTECONTROL_CPP_PATH`, `SRC_MAINWINDOW_CPP_PATH` compile
defs already declared on `test_claude`.

## Out of scope

- Runtime accuracy of the substring match (no symbol resolution by
  design; v1 is path-substring only).
- Cross-spec dedup of invariants when two specs share an INV-N
  identifier — each spec's invariants are returned independently.
