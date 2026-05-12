# mcp_workspace_search — ANTS-1248

Locks the wiring contract for the new `workspace_search` MCP tool —
a ripgrep wrapper that replaces a typical `Bash grep -r ... src/`
pattern with structured `{matches[], truncated, elapsed_ms}` results.

Spec of record: `docs/specs/ANTS-1248.md`. This test file pins the
source-grep invariants only; runtime/process behaviour of ripgrep
itself is the kernel of the ANTS-1248 cold-eyes pass and is left to
manual smoke-test (no need to spawn a child process from the test
suite — adds ~200 ms latency per ctest cycle for a deterministic
shape we can grep for).

## Invariants

| # | Statement |
|---|-----------|
| 1 | `cmdWorkspaceSearch(const QJsonObject &req)` is declared public on `RemoteControl` in `src/remotecontrol.h`, alongside the ANTS-1244 trio (`cmdRoadmapQuery`/`cmdTabList`/`cmdGetText`). |
| 2 | `cmdWorkspaceSearch` is defined in `src/remotecontrol.cpp` and carries the 10 invariant anchors `// ANTS-1248-INV-1` through `// ANTS-1248-INV-10` (one per spec invariant). Each anchor sits next to the code that enforces it. |
| 3 | `rg` is invoked with shell-less argv — `QProcess::start(\"rg\", QStringList...)` (the two-argument form), **not** `QProcess::start(\"bash -c ...\")`, **not** the single-string `start(\"...\")` overload, **not** `system(`, **not** `popen(`. Source-grep negative check. |
| 4 | The IPC dispatcher in `RemoteControl::dispatch` routes `\"workspace-search\"` to `cmdWorkspaceSearch`. |
| 5 | The MCP `tools/list` block in `src/claudeintegration.cpp` registers a `\"workspace_search\"` entry with an `inputSchema` that declares `properties.pattern` (required) plus `regex`, `lane`, `glob`, `max_results`, `context`, `case`. ANTS-1256 parity: the schema is not the shared `emptySchema`. |
| 6 | The MCP `tools/call` dispatcher in `src/claudeintegration.cpp` has an `else-if (toolName == \"workspace_search\" && m_workspaceSearchProvider)` clause that extracts `arguments` and forwards. |
| 7 | `ClaudeIntegration` declares `setWorkspaceSearchProvider(std::function<QString(const QJsonObject&)>)` in `src/claudeintegration.h` and a matching `m_workspaceSearchProvider` member. Signature is the full-`QJsonObject` shape (matches `cmdGetText` provider widening idiom). |
| 8 | `MainWindow::setupClaudeMcpProviders` in `src/mainwindow.cpp` calls `setWorkspaceSearchProvider` with a lambda that delegates to `m_remoteControl->cmdWorkspaceSearch`. Falls back to the same `\"remote-control unavailable\"` JSON when `m_remoteControl` is null. |
| 9 | The body uses ripgrep flags `--json`, `--no-heading`, `--line-number`, `--max-columns 500`, `--threads 1`. These appear in the argv literally. |
| 10 | Hard kill is wired via `QTimer::singleShot(2000` (or equivalent constant naming) sending `terminate()`, then a 200 ms grace before `kill()` — INV-5 in the spec. |

## Acceptance

Exit 0 = all 10 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. No per-feature `CMakeLists.txt`. Uses
the existing `SRC_CLAUDE_INTEGRATION_CPP_PATH`,
`SRC_CLAUDE_INTEGRATION_H_PATH`, `SRC_RC_HEADER`,
`SRC_REMOTECONTROL_CPP_PATH`, `SRC_MAINWINDOW_CPP_PATH` compile
defs already declared on `test_claude`.

## Out of scope

- Runtime correctness of the ripgrep child process. Manual smoke
  during /release; the ANTS-1248 spec's perf canary covers this
  if it ever lands as an automated test.
- Path-traversal / regex-DoS exploit attempts — the spec already
  audited these surfaces via cold-eyes; no fuzz harness here.
- Output-shape JSON validation. The `{matches[], truncated,
  elapsed_ms}` envelope is enforced by the body's own structure;
  source-grep covers the field names.
