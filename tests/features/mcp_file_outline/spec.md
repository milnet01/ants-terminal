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
| 2 | `cmdFileOutline` body in the remotecontrol TUs carries `// ANTS-1249-INV-` anchors for INV-1, INV-2, INV-10. (The remaining INV-3 to INV-9 anchors live in `fileoutline.cpp` because that is where the regex set + header-doc cap + line cap actually fire.) |
| 3 | `src/fileoutline.cpp` contains the six regex builders — `rxCppMember`, `rxCppType`, `rxCppFunc`, `rxCppQt`, `rxPy`, `rxMdHeading` — each a `static const QRegularExpression` with `.optimize()` called once. The per-line byte cap (`kMaxLineBytes`) and header-doc cap (`kHeaderDocByteCap`) are present. |
| 4 | The IPC dispatcher in `RemoteControl::dispatch` routes `"file-outline"` to `cmdFileOutline`. |
| 5 | The MCP `tools/list` block in `src/claudeintegration.cpp` registers a `"file_outline"` entry with an `inputSchema` declaring `path` + `mode`, `include_doc_comment`, `max_symbols`, and the ANTS-2223 multi-path props `paths` + `etags`. `path` is no longer in `required` (the `paths` form satisfies the verb without it). |
| 6 | The MCP `tools/call` dispatcher has an `else-if (toolName == "file_outline" && m_fileOutlineProvider)` clause. |
| 7 | `ClaudeIntegration` declares `setFileOutlineProvider(std::function<QString(const QJsonObject&)>)` in `src/claudeintegration.h` plus matching `m_fileOutlineProvider` member. |
| 8 | `MainWindow::setupClaudeMcpProviders` calls `setFileOutlineProvider` with a lambda that delegates to `m_remoteControl->cmdFileOutline`. |
| 9 | `FileOutline::compute` (called against the in-tree `src/auditdialog.cpp`) returns `≥ 8` symbols. Smoke-test of the regex set against a known file — flips red if the regex set ever regresses. |
| 10 | Calling `FileOutline::compute` on a non-existent path returns `{ok:false, code:"not_found"}` and does not crash. |
| 11 | (ANTS-2028) `FileOutline::compute` captures free functions with a single-token return type — `int alpha()`, `static QByteArray slurpBody(...)`, and the declaration `const std::string &makeName(int);` all surface — while the qualified member `Widget::method` still resolves via `rxCppMember`. Guards against `rxCppFunc` folding the return type and the name into one possessive class (which left nothing for the name capture, so free functions never matched). |
| 12 | (ANTS-2147) `FileOutline::compute` does not emit a statement-position call as a function symbol — `return gamma(7);` surfaces no `gamma` symbol — while the enclosing free function `int beta()` still surfaces. `rxCppFunc` rejects an expression-introducing reserved keyword (`return`/`co_return`/`co_await`/`co_yield`/`throw`/`else`) as the leading return-type token. |
| 13 | (ANTS-2223) `cmdFileOutline` in the remotecontrol TUs supports the multi-path form: the single-path and multi-path bodies share the extracted `outlineOneFile()` helper; the handler branches on a `paths` array (`pathsVal.isArray()`); each entry carries a per-file etag via `outlineFileEtag()`; an optional `etags` map 304s an unchanged entry to an `unchanged` stub; and the batch envelope emits `files[]` + `count`. (Locked at source level — the handler needs a live `MainWindow` to invoke, as with INV-2/INV-4.) |

## Acceptance

Exit 0 = all 13 invariants hold.

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
