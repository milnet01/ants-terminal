# mcp_test_results — ANTS-1300

Locks the wiring contract for the `test_results` MCP tool — the
op-dispatched `record` / `read` surface over
`<project>/.audit_cache/tests.json`, with `detail=<name>` mode for
returning one failing test's full excerpt.

## Invariants

| # | Statement |
|---|-----------|
| 1 | `TestResCache::cachePath`, `TestResCache::parseCtestOutput`, `TestResCache::recordTests`, `TestResCache::loadTests`, `TestResCache::toJsonWire` declared in `src/testrescache.h`. |
| 2 | `cmdTestResults(const QJsonObject &req)` declared public on `RemoteControl` in `src/remotecontrol.h`. |
| 3 | `cmdTestResults` defined in `src/remotecontrol.cpp` and carries an `ANTS-1300` anchor comment. |
| 4 | The body dispatches on `op` ∈ `{record, read}`, refuses `detail` arg on `op=record`, surfaces `bad_args` for unknown ops / missing args / unparseable output / empty `output`. |
| 5 | The body refuses with `code:"not_cached"` when `op=read` and no cache exists, `code:"detail_not_found"` when `detail=<name>` doesn't match any failing test, and `code:"write_failed"` when `op=record` cannot persist. |
| 6 | The body uses `toJsonWire` on the default read path so the wire envelope omits the on-disk `full_excerpt` field; the `detail` path returns `{name, excerpt}` with the full body. |
| 7 | `MainWindow::setupClaudeMcpProviders` registers `"test_results"` via `registerToolProvider` and delegates to `m_remoteControl->cmdTestResults`. Falls back to `kRcUnavailable` when `m_remoteControl` is null. |
| 8 | The `tools/list` block in `src/claudeintegration.cpp` registers a `"test_results"` entry. Schema declares `properties.caller_cwd` (required) and offers `op`, `exit_code`, `output`, `started_at_ms`, `finished_at_ms`, `duration_ms`, `detail`, `etag_match` as optional properties. |
| 9 | `callerCwdContractFor` classifies `"test_results"` as `Required` (explicit branch). |
| 10 | `kindForName` returns `"test"` for `"test_results"` (new bucket branch). |
| 11 | `isEtagSupportedTool` returns true for `"test_results"`. |
| 12 | Token-cost ledger in `claudeintegration.cpp` carries `{QStringLiteral("test_results"), {800, 3000}}`. |

## Acceptance

Exit 0 = all 12 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. Re-uses
`SRC_CLAUDE_INTEGRATION_CPP_PATH`, `SRC_RC_HEADER`,
`SRC_REMOTECONTROL_CPP_PATH`, `SRC_MAINWINDOW_CPP_PATH`; needs a
new `SRC_TESTRESCACHE_H_PATH` compile def.

## Out of scope

- Ctest-output regex matching against real-world ctest variants
  (the regex is verified against this project's own ctest output;
  cross-distro variants out of scope).
- Per-line / per-excerpt cap correctness — bounded by the regex
  + clampExcerpt helper, exercised at runtime not at build-time.
