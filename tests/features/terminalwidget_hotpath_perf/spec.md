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

- **INV-4 / search-match predicate hoisted per row, computed once per
  cell.** ANTS-1841 removed the double per-cell probe (colour + opacity
  each ran `isCellSearchMatch`). ANTS-3457 goes further: the paint cell
  loop MUST NOT call `isCellSearchMatch(globalLine, col)` at all — the
  underlying `std::lower_bound` into `m_searchMatches` is precomputed
  once per row into `m_paintSearchSpans`, and the per-cell result is a
  single `const bool searchMatch` walk of that (typically empty) row-span
  list, shared between the colour and opacity decisions. A future edit
  that reintroduces the per-cell `isCellSearchMatch(globalLine, col)`
  probe (O(cols·log matches) per frame) reverts the fix.

- **INV-5 / paint honours the damage rect** (ANTS-3454). `paintEvent` reads
  `event->rect()` and starts its row loop at the first damaged row.

- **INV-6 / the suggestion scan does not detach history** (ANTS-4780).
  `updateSuggestion` iterates `std::as_const(m_historyEntries)`.

- **INV-7 / no highlight cache without rules** (ANTS-5077). In `paintEvent`
  the `if (!m_highlightRules.empty())` guard precedes `m_hlSpanCache.find(`,
  so a session with no rules adds no cache entry per painted line.

- **INV-8 / background drawn, not stretched** (ANTS-5077). The pre-scaled
  background image is drawn through a source rect
  (`p.drawImage(rect(), m_backgroundImage, …)`), not stretched onto `rect()`.

- **INV-9 / key log records no typed text** (ANTS-5077). `keyPressEvent`'s
  debug log records the text's length, not the text (`text=%s` is gone).

- **INV-10 / durations never negative** (ANTS-5077). No command duration is
  computed as a bare `pr.commandEndMs - pr.commandStartMs`; both display sites
  clamp at zero.

- **INV-11 / navigation updates the scroll bar** (ANTS-5078).
  `scrollToMatch`, `nextBookmark` and `prevBookmark` each call
  `updateScrollBar()`.

- **INV-12 / right-click bounds the selection first** (ANTS-5078).
  `contextMenuEvent` computes `selCellBound` before calling `selectedText()`.

- **INV-13 / recording decodes across batches** (ANTS-5078). `onVtBatch`
  decodes through `m_recordDecoder`, not `QString::fromUtf8` per batch.

- **INV-14 / rich copy is bounded** (ANTS-5078). `copySelectionRich` writes
  one span per run of same-style cells (`flushRun()`) and copies plain text
  only when the selection's bounds exceed `kRichCopyCellCap` cells.

- **INV-15 / line triggers defer cache invalidation** (ANTS-5078).
  `onGridLineCompleted` runs from the grid's line-completion callback, inside
  a VT batch. It sets `m_spanCacheDirty` rather than calling
  `invalidateSpanCaches`, which would clear every row's dirty flag and
  `m_spanCacheDirty` mid-batch and leave rows written later in that batch
  with stale URL and highlight spans.

## Test scope

Source-scrape against `src/terminalwidget.cpp` via `SRC_TERMINALWIDGET_PATH`.
No GUI / event-loop / paint instantiation. Each invariant fails fast if a
future edit reverts the corresponding fix.
