# A copy right after a select reuses the selection's text

ANTS-5791. Releasing the mouse on a selection builds its text once, for
the primary selection (`publishSelection`). Ctrl+Shift+C then built it a
second time; over a full 50k-line scrollback each build takes hundreds of
milliseconds (`bench_selection_copy`). `TerminalWidget::selectedText()`
now keeps the last whole-selection text and returns it while the grid's
`contentRevision()`, the selection's two ends and its rectangular flag
are unchanged.

The test builds a real widget, prints a word, and double-clicks it.

## Invariants

- **INV-1 — an unchanged selection is not rebuilt.** Two `selectedText()`
  calls in a row return the same string buffer (`constData()` equal), so
  the second built nothing.
- **INV-2 — a grid change rebuilds it.** After output overwrites the
  selected cells, `selectedText()` returns the new text, not the cached
  one.

## Not covered

The time saved is measured by `tests/perf/bench_selection_copy.cpp`, which
is outside the default test presets.
