# Terminal search scan — results are fixed by the grid, not by how the scan reads it

ANTS-2000. `TerminalWidget::performSearch` matches the user's pattern against
each line's text, built from the grid's cells. Making that build faster must
not change what a search finds. This contract pins what it finds.

The test drives the real widget the way a user does: text in the search bar,
then the regex toggle, which scans at once. It reads the match total from the
bar's `current/total` label.

## Invariants

- **INV-1 — scrollback and screen are both searched.** A needle printed on a
  line that has scrolled into history, and again on a visible line, is found
  on both.
- **INV-2 — a combining mark follows its base character.** `e` then U+0301
  on a line matches the pattern `e\x{0301}`.
- **INV-3 — a character outside the BMP is one character.** U+1D400, which
  occupies one cell, followed by `y` matches the pattern `\x{1D400}y`.
- **INV-4 — a wide character's continuation cell reads as a space.** Two
  adjacent wide characters match the pattern with a space between them.
- **INV-5 — a line reads as the full grid width.** A history line can store
  fewer cells than the grid is wide: `TerminalGrid::pushScrollbackLine`, the
  session-restore entry, keeps a saved line at its saved width. The cells past
  what it stores read as spaces, so `short +$` matches a stored `short`.
  `TerminalGrid::resize` pads every history line, so a resize cannot reach
  this case.

## Test

`test_terminal_search_scan.cpp`. Each invariant is one assertion on the match
total. The test pins behaviour that already exists, so it passes before the
ANTS-2000 change; it is proven able to fail by mutating the scan.
