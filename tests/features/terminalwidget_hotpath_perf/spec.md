# Feature spec: terminalwidget hot-path hygiene (ANTS-1841)

Indie-review #6 (2026-05-22) found four wasteful-repeated-work spots on
`TerminalWidget`'s interactive hot paths. None change visible behaviour;
each removes redundant per-event or per-cell work. This test locks the
post-fix source shape so a future edit can't silently reintroduce the
cost.

These are GUI / paint-path concerns that can't be driven headlessly, so
the invariants are source-scrape assertions against
`src/terminalwidget.cpp` (the established pattern — see
`scrollback_frozen_view`).

## Invariants

- **INV-1 / hover hit-test uses the span cache.** `mouseMoveEvent` runs
  on every pixel of motion. It MUST resolve URL spans via
  `urlSpansForLine(...)` (the cached front-end), not by calling
  `detectUrls(...)` directly — the latter re-ran the URL regex for the
  hovered line on every move. The cache is what the last paint left, i.e.
  what the user sees, so it is also the correct hit-test source.

- **INV-2 / history dedup is not O(n²).** `loadHistory` MUST dedup with a
  `QSet<QString>` membership check, NOT `m_historyEntries.contains(...)`
  (a linear scan per line → O(n²) on a large shell history). Ordering is
  unchanged: first occurrence keeps its most-recent-first prepend slot.

- **INV-3 / triple-click selects in linear mode.** The triple-click
  branch returns before the fall-through `m_rectSelection = false`, so it
  MUST clear `m_rectSelection` itself. Otherwise an Alt-drag (which sets
  the rect flag) followed by a triple-click on the same cell runs a
  full-line selection under rectangular-selection semantics.

- **INV-4 / search-match predicate computed once per cell.** The paint
  cell loop MUST evaluate `isCellSearchMatch(globalLine, col)` at most
  once per cell (hoisted into a local), not once for the colour decision
  and again for the opacity decision. Under opacity + an active search
  the duplicate was a second binary-search probe on every painted cell.

## Test scope

Source-scrape against `src/terminalwidget.cpp` via `SRC_TERMINALWIDGET_PATH`.
No GUI / event-loop / paint instantiation. Each invariant fails fast if a
future edit reverts the corresponding fix.
