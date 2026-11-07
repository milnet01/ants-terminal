# Feature: ANTS-4534 — a per-tab scrollback memory cap

Each tab's scrollback has a size limit as well as a line limit. Once the
cells it stores pass the limit, the oldest lines are dropped until they fit.
The limit is `scrollback_max_mib` in `config.json`, default 256 MiB, and is
set in Settings → Terminal. It counts cell storage (`sizeof(Cell)` per stored
cell), not each line's fixed header or its combining-character table.

User decision 2026-11-07 on ANTS-4534: a real ceiling, whatever the output,
rather than a documented typical figure.

**Reload.** A change to `config.json` or to the Settings dialog reaches every
open tab through `MainWindow::applyConfigToTerminal`; no relaunch. The cap
holds no running state beyond its number, so it has no stop/start control.

## Invariants

- **INV-1 — the cap holds.** With the cap set, `TerminalGrid::scrollbackBytes()`
  never exceeds `maxScrollbackMib()` MiB after output, and the oldest lines are
  the ones dropped. Broken by evicting on the line limit only.
  *Test:* `ScrollbackMemoryCap.Inv1CapHoldsAndDropsOldest`.
- **INV-2 — lowering the cap trims at once.** `setMaxScrollbackMib` with a
  smaller value evicts down to it before returning. Broken by applying the
  cap only on the next push. *Test:* `ScrollbackMemoryCap.Inv2LoweringTrimsAtOnce`.
- **INV-3 — the count is exact.** `scrollbackBytes()` equals the sum of
  `scrollbackLine(i).size() * sizeof(Cell)` after pushes, a resize, an
  `ED 3` clear and a reset (`ESC c`). Broken by a mutation path that skips
  the count. *Test:* `ScrollbackMemoryCap.Inv3CountMatchesStorage`.
- **INV-4 — a reset keeps the cap.** `ESC c` leaves `maxScrollbackMib()` at
  its configured value, as it does the line limit. *Test:*
  `ScrollbackMemoryCap.Inv4ResetKeepsTheCap`.
- **INV-5 — range and default.** The cap is clamped to 16 to 65,536 MiB;
  `Config::scrollbackMaxMib()` defaults to 256. Broken by an unclamped value.
  *Test:* `ScrollbackMemoryCap.Inv5RangeAndDefault`.
- **INV-6 — wired to every tab.** `MainWindow::applyConfigToTerminal` passes
  `scrollbackMaxMib()` to each terminal, and the Settings dialog loads and
  saves it. *Test:* `ScrollbackMemoryCap.Inv6WiredThroughConfigAndSettings`
  (source scrape).

## Tests

- `test_scrollback_memory_cap.cpp` (on the `test_vt` bundle) — INV-1 to INV-6.
