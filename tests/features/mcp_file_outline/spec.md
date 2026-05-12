# mcp_file_outline — ANTS-1249

Locks the wiring contract for the new `file_outline` MCP tool — a
regex-scanner outline returning `{header_doc, symbols[], total_lines,
total_bytes}` for a single file. ~13-39× compression vs full Read on
typical C++ source files.

Spec of record: `docs/specs/ANTS-1249.md`. This test file pins the
source-grep wiring contract plus a tiny runtime smoke against the
in-tree `auditdialog.cpp` (INV-3 floor check).

## Invariants

| # | Statement |
|---|-----------|
| 1 | `cmdFileOutline(const QJsonObject &req)` is declared public on `RemoteControl` in `src/remotecontrol.h`. |
| 2 | `cmdFileOutline` body in `src/remotecontrol.cpp` carries `// ANTS-1249-INV-` anchors for INV-1, INV-2, INV-10. (The remaining INV-3 to INV-9 anchors live in `fileoutline.cpp` because that is where the regex set + header-doc cap + line cap actually fire.) |
| 3 | `src/fileoutline.cpp` contains the six regex builders — `rxCppMember`, `rxCppType`, `rxCppFunc`, `rxCppQt`, `rxPy`, `rxMdHeading` — each a `static const QRegularExpression` with `.optimize()` called once. The per-line byte cap (`kMaxLineBytes`) and header-doc cap (`kHeaderDocByteCap`) are present. |
| 4 | The IPC dispatcher in `RemoteControl::dispatch` routes `"file-outline"` to `cmdFileOutline`. |
| 5 | The MCP `tools/list` block in `src/claudeintegration.cpp` registers a `"file_outline"` entry with an `inputSchema` declaring `path` (required) + `mode`, `include_doc_comment`, `max_symbols`. |
| 6 | The MCP `tools/call` dispatcher has an `else-if (toolName == "file_outline" && m_fileOutlineProvider)` clause. |
| 7 | `ClaudeIntegration` declares `setFileOutlineProvider(std::function<QString(const QJsonObject&)>)` in `src/claudeintegration.h` plus matching `m_fileOutlineProvider` member. |
| 8 | `MainWindow::setupClaudeMcpProviders` calls `setFileOutlineProvider` with a lambda that delegates to `m_remoteControl->cmdFileOutline`. |
| 9 | `FileOutline::compute` (called against the in-tree `src/auditdialog.cpp`) returns `≥ 8` symbols. Smoke-test of the regex set against a known file — flips red if the regex set ever regresses. |
| 10 | Calling `FileOutline::compute` on a non-existent path returns `{ok:false, code:"not_found"}` and does not crash. |

## Acceptance

Exit 0 = all 10 invariants hold.

Wired as a source file in the `test_claude` bundle (uses the same
compile defs as the existing MCP-related tests). The runtime
checks (INV-9, INV-10) call `FileOutline::compute` directly — the
test must link against `ants_core_lib` which already provides
`fileoutline.cpp`.

## Out of scope

- Python / Markdown floor numbers — INV-9 covers the C++ path which
  is the high-leverage case. Per-language fixtures land if/when a
  regression is observed.
- Path-escape exploits — `pathInRepoRoot` logic lives in
  `cmdFileOutline`, exercised by INV-1 in remotecontrol.cpp. The
  spec already audited this via cold-eyes.
- Performance canary — the 25 ms / 5 000-line target is aspirational;
  perf regression detection would need a benchmark harness, not a
  unit test.
