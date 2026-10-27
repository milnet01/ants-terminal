# Span cell columns — a span found in a line's text lands on the cells that hold it

`TerminalWidget::lineText` builds a line's text from its cells: a codepoint
outside the BMP is two UTF-16 units, a cell's combining marks follow its base
character, and a wide character's continuation cell reads as a space. A regex
offset into that text is therefore not a cell column. URL and file-path spans
(`detectUrls`), highlight-rule spans, search matches (`performSearch`),
quick-select labels and the `highlight_text` / `make_hyperlink` triggers all
find their span in that text, then paint, hit-test (Ctrl+click) or mutate the
grid by cell column. Each converts its offsets through
`TerminalWidget::lineColumns`, which maps every UTF-16 unit of the text, and
one past the end, to its cell. `lineText`'s output is unchanged
(`tests/features/terminal_search_scan`).

The test feeds lines through the real parser into a real widget, then reads
the spans' columns through the `urlSpanColumnsForTest` and
`searchMatchColumnsForTest` probes. The expected columns come from the grid
itself: the cells whose codepoints spell the span.

## Invariants

- **INV-1 — a URL after an emoji starts and ends on its own cells.** On
  `😀 see https://a.example/x`, the URL span's columns are the cells holding
  `h` through `x`. U+1F600 is two UTF-16 units.
- **INV-2 — a URL after a one-cell non-BMP character is aligned.** On
  `x𝐀 https://b.example/y` (U+1D400, one cell, two units) the same holds.
- **INV-3 — a search match after a combining mark is aligned.** On
  `cafe` + U+0301 + ` NEEDLE`, the match for `NEEDLE` covers the six cells
  that hold it, not cells one to the right.
- **INV-4 — a search match ending on a wide character covers both of its
  cells.** On `a世b`, a match for `a世` ends on the wide character's
  continuation cell.

## Test

`test_span_cell_columns.cpp` in the `test_chrome` bundle. INV-1 to INV-3 fail
against the code before the change (spans one or more cells to the right);
INV-4 failed with the match ending on the wide character's first cell.
