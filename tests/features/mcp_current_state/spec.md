# mcp_current_state — ANTS-1569

Locks the wiring contract for the `current_state` MCP aggregator —
the one-call session-start state recovery verb that bundles
`roadmap_query` + `git_state(status)` + `last_audit_summary` +
`.claude/workflow.md` parse + `docs/specs/<active-id>.md` probe.

Spec of record: `docs/specs/ANTS-1569.md` (14 INVs). This test pins
the source-grep invariants only; runtime behaviour of the aggregator
is left to manual smoke (the upstream verbs each have their own
runtime test).

## Invariants

| # | Statement |
|---|-----------|
| 1 | `cmdCurrentState(const QJsonObject &req)` declared public on `RemoteControl` in `src/remotecontrol.h`. |
| 2 | `cmdCurrentState` defined in `src/remotecontrol.cpp` and carries an `ANTS-1569` anchor comment in or above the function body. |
| 3 | The body delegates to `cmdRoadmapQuery`, `cmdGitState`, and `cmdLastAuditSummary` — all three names appear in the body of `cmdCurrentState`. |
| 4 | MCP-only: no `cmd == "current-state"` branch in `RemoteControl::dispatch`. Mirror of `last_audit_summary`. |
| 5 | `MainWindow::setupClaudeMcpProviders` (`src/mainwindow.cpp`) registers `"current_state"` via `registerToolProvider` and delegates to `m_remoteControl->cmdCurrentState`. Falls back to `kRcUnavailable` when `m_remoteControl` is null. |
| 6 | The `tools/list` block in `src/claudeintegration.cpp` registers a `"current_state"` entry. Schema declares `properties.caller_cwd` (required) and `properties.etag_match` (optional), both built via the shared helpers (`makeCallerCwdReadProp` and `makeEtagMatchProp` appear in the same registration block). |
| 7 | `callerCwdContractFor` in `src/claudeintegration.cpp` classifies `"current_state"` as `Required` (explicit branch, declarative parity with sibling project-scoped tools). |
| 8 | `isEtagSupportedTool` in `src/claudeintegration.cpp` returns true for `"current_state"` (joins the ANTS-1499 304 group). |
| 9 | The tools/list description mentions the four upstream sources by name: `roadmap_query`, `git_state`, `last_audit_summary`, and the `.claude/workflow.md` file. |
| 10 | The handler returns `{ok:true, …}` for every successful call — no `ok:false` escalation on upstream failure (INV-14 of the spec). Source-grep: only one `csErr(QStringLiteral(...` call site emits `code:"no_project"` for the project-root-unresolved case; no other refusal-code call sites exist in `cmdCurrentState`. |

## Acceptance

Exit 0 = all 10 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. No per-feature `CMakeLists.txt`. Uses
the existing `SRC_CLAUDE_INTEGRATION_CPP_PATH`,
`SRC_RC_HEADER`, `SRC_REMOTECONTROL_CPP_PATH`,
`SRC_MAINWINDOW_CPP_PATH` compile defs already declared on
`test_claude`.

## Out of scope

- Runtime correctness of the aggregator. Manual smoke during
  `/release`; the spec's three upstreams each carry their own
  runtime feature tests.
- `.claude/workflow.md` parser robustness (best-effort by design).
- Etag invalidation under simultaneous upstream change.
