# mcp_build_status — ANTS-1299

Locks the wiring contract for the `build_status` MCP tool — the
op-dispatched `record` / `read` surface over
`<project>/.audit_cache/build.json`.

This test pins source-grep invariants only; runtime parser
behaviour is exercised indirectly through the build (the
`parseBuildOutput` helper is wired into the MCP dispatch path and
its existence is asserted as a wiring contract).

## Invariants

| # | Statement |
|---|-----------|
| 1 | `BuildCache::cachePath`, `BuildCache::parseBuildOutput`, `BuildCache::recordBuild`, `BuildCache::loadBuild`, `BuildCache::checkStale` declared in `src/buildcache.h`. |
| 2 | `cmdBuildStatus(const QJsonObject &req)` declared public on `RemoteControl` in `src/remotecontrol.h`. |
| 3 | `cmdBuildStatus` defined in the remotecontrol TUs and carries an `ANTS-1299` anchor comment in or above the function body. |
| 4 | The body dispatches on `op` ∈ `{record, read}` and surfaces `bad_args` for unknown ops, missing `exit_code` / missing `output` on record, or empty `output`. |
| 5 | The body refuses with `code:"not_cached"` when `op=read` and no cache exists, and with `code:"write_failed"` when `op=record` cannot persist the cache. |
| 6 | `MainWindow::setupClaudeMcpProviders` (`src/mainwindow.cpp`) registers `"build_status"` via `registerToolProvider` and delegates to `m_remoteControl->cmdBuildStatus`. Falls back to `kRcUnavailable` when `m_remoteControl` is null. |
| 7 | The `tools/list` block in `src/claudeintegration.cpp` registers a `"build_status"` entry. Schema declares `properties.caller_cwd` (required) and offers `op`, `exit_code`, `output`, `started_at_ms`, `finished_at_ms`, `etag_match` as optional properties. |
| 8 | `callerCwdContractFor` classifies `"build_status"` as `Required` (explicit branch). |
| 9 | `kindForName` returns `"build"` for `"build_status"` (new bucket branch). |
| 10 | `isEtagSupportedTool` returns true for `"build_status"`. |
| 11 | Token-cost ledger in `claudeintegration.cpp` carries `{QStringLiteral("build_status"), {500, 2000}}`. |

## Acceptance

Exit 0 = all 11 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. Re-uses the existing
`SRC_CLAUDE_INTEGRATION_CPP_PATH`, `SRC_RC_HEADER`,
`ANTS_RC_SOURCES`, `SRC_MAINWINDOW_CPP_PATH` compile
defs; needs a new `SRC_BUILDCACHE_H_PATH` compile def added in the
same block.

## Out of scope

- Runtime correctness of the parser (regex hits, error-line capture,
  continuation folding) — exercised via the build and the
  reusable-from-MCP code path; not source-grep testable.
- Stale-mtime walk correctness — bounded by the 5 000-file cap; the
  walk is exercised at runtime, not at build-time.
- Etag injection mechanics — covered by the shared ANTS-1499 helper
  already tested in sibling features.
