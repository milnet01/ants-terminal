# mcp_spec_query — ANTS-1309

Locks the wiring contract for the `spec_query` MCP tool — the
token-frugal per-spec parser that returns `{title, status, kind,
invariants[]}` without forcing the caller to Read the full
~2 K-line markdown body.

This test pins source-grep invariants only; runtime parser
behaviour is exercised indirectly through `invariant_check`
(which uses the same `parseSpecBody` helper).

## Invariants

| # | Statement |
|---|-----------|
| 1 | `cmdSpecQuery(const QJsonObject &req)` declared public on `RemoteControl` in `src/remotecontrol.h`. |
| 2 | `cmdSpecQuery` defined in `src/remotecontrol.cpp` and carries an `ANTS-1309` anchor comment in or above the function body. |
| 3 | The body validates a passed `id` argument (rejects malformed ids with `code:"bad_id"`). |
| 4 | The body refuses with `code:"not_found"` when the resolved spec file does not exist. |
| 5 | The body returns an `invariants` array (parsed via the shared `parseSpecBody` helper). |
| 6 | `MainWindow::setupClaudeMcpProviders` (`src/mainwindow.cpp`) registers `"spec_query"` via `registerToolProvider` and delegates to `m_remoteControl->cmdSpecQuery`. Falls back to `kRcUnavailable` when `m_remoteControl` is null. |
| 7 | The `tools/list` block in `src/claudeintegration.cpp` registers a `"spec_query"` entry whose schema marks `caller_cwd` required but NOT `id` — `id` is optional (ANTS-1906 `path` escape; ANTS-3360 list mode). Scoped to the registration block, not a fixed byte window. |
| 8 | `callerCwdContractFor` in `src/claudeintegration.cpp` classifies `"spec_query"` as `Required` (explicit branch). |
| 9 | ANTS-3360 list mode: `cmdSpecQuery` delegates the no-`id`/no-`path` case to the `specListEnvelope` helper; the list-mode code carries an `ANTS-3360` anchor. |
| 10 | ANTS-3356 generalised id routing: any `<PREFIX>-NNNN` id resolves via the shared `resolveSpecRelForId` helper (exact `<id>.md`, then a `<id>-*.md` glob for topic-suffixed specs); carries an `ANTS-3356` anchor. |

## Acceptance

Exit 0 = all invariants hold (enforced by a final `EXPECT_EQ(0,
expect_failures())` — previously omitted, which left the source-grep
INVs counted but unasserted).

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. Uses the existing
`SRC_CLAUDE_INTEGRATION_CPP_PATH`, `SRC_RC_HEADER`,
`SRC_REMOTECONTROL_CPP_PATH`, `SRC_MAINWINDOW_CPP_PATH` compile
defs already declared on `test_claude`.

## Out of scope

- Runtime correctness of the parser. Exercised indirectly via the
  sibling `mcp_invariant_check` test, which feeds a fixture spec
  through the shared `parseSpecBody` helper.
- Behaviour on specs that have no Invariants section (returns empty
  array — a parser detail, not a contract).
