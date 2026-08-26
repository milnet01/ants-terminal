# Feature test — `read_region` MCP tool (ANTS-2021)

Behavioural invariants drive the pure `ReadRegion::extract` helper; wiring
invariants source-scrape the registration sites. Full design contract:
[`docs/specs/ANTS-2021.md`](../../../docs/specs/ANTS-2021.md).

| # | Test | Asserts |
|---|------|---------|
| W1 | `WiringContract` | `cmdReadRegion` declared + calls `ReadRegion::extract` + `validatePath`; `read_region` registered in claudeintegration (schema + `C::Required` contract + `isEtagSupportedTool`); present in mcpprojection's compaction table; `registerToolProvider("read_region"` in mainwindow. |
| B1 | `LineRange` (INV-1) | line mode returns exactly `[start,end]` in order; `end` clamps to EOF; effective range echoed. |
| B2 | `PastEof` (INV-1) | `start_line` past EOF → `ok:true`, `lines:[]`. |
| B3 | `SymbolBody` (INV-2) | symbol mode resolves a leaf function's body `[line, nextSymbolLine)`; echoes `symbol`. |
| B4 | `SymbolNotFound` (INV-2) | unknown symbol → `code:"symbol_not_found"`. |
| B5 | `SelectorExclusivity` (INV-3) | neither / both selectors → `bad_args`; `start<1` / `end<start` → `bad_args`. |
| B6 | `ByteCapHead` (INV-8) | small `max_bytes` keeps the head, stops early, sets `truncated`, effective `end_line` < requested; over-ceiling sets `bytes_cap_clamped`. |
| B7 | `Ants4700PerLineClipMakesRoomForMoreLines` | `max_line_bytes` clips each line BEFORE the byte cap is charged, so the same `max_bytes` reaches strictly MORE lines than without it — the assertion is against the unclipped call's own `returned`, because a clip that only shortened what already fitted would satisfy a fixed number. Echoes `max_line_bytes` / `lines_clipped`; the marker is U+2026, as `max_match_bytes` emits it; a line that already fitted is untouched. |
| B8 | `Ants4700ClampsAndReportsTheLineCap` | An out-of-range `max_line_bytes` is clamped to [50, 10000] and says so via `line_cap_clamped`. A guard on the echoed contract rather than a red-to-green case: it passes with the clip disabled, because clamping is plumbing. It also asserts clamping does not INVENT clipping (`lines_clipped: 0` on short lines). |

INV-4 (path validation), INV-5 (caller_cwd Required), INV-6 (ETag), INV-7
(fields projection), INV-9 (bounded read) are covered by the W1 source-scrape
(registration-presence) — the pure helper emits no path/etag itself.

Label: `features;fast`. Verify each fails against pre-implementation source.
