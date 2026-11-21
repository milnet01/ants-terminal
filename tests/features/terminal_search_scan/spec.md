# Terminal search scan — results are fixed by the grid, not by how the scan reads it

ANTS-2000. `TerminalWidget::performSearch` matches the user's pattern against
each line's text, built from the grid's cells. Making that build faster must
not change what a search finds. This contract pins what it finds.

The test drives the real widget the way a user does: text in the search bar,
then the regex toggle, which starts the scan at once. It reads the match total from the
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
- **INV-5 — the cells past what a line stores read as blanks.** A history
  line can store fewer cells than the grid is wide:
  `TerminalGrid::pushScrollbackLine`, the session-restore entry, keeps a saved
  line at its saved width. The cells past it read as spaces, so INV-7 drops
  them and `short$` matches a stored `short`. `TerminalGrid::resize` pads
  every history line, so a resize cannot reach this case.
- **INV-6 — a zero-width match does not end the line's scan** (ANTS-5078).
  `(?=o)|one` finds an empty match at `o` before `one` on the same line; the
  `one` match is still counted.
- **INV-7 — a search ignores the blanks after the end of each line.** A
  query of spaces does not match the empty cells that pad a line to the grid
  width, so ` $` and `short +$` match nothing, and `NEEDLE one$` matches
  because the line now ends at its last character. The cost, accepted by the
  user on 2026-09-12: trailing spaces a program really printed cannot be
  found.
- **INV-8 — a search keeps at most 100000 matches, the newest** (ANTS-6494).
  Past the cap it keeps the matches nearest the end of the output and the
  counter's total reads `100000+`. 600 lines of 200 `a`s searched for `a`
  read `99801/100000+`: the current match is the first on the view's centre
  row, the second-to-last line printed.
- **INV-9 — a long search yields, and ends where a whole one does**
  (ANTS-5418). A search scans a bounded number of cells, newest lines first,
  then returns to the event loop and goes on in the next turn. While it runs
  the counter's total ends in `…`. A search run in five-line slices first
  shows `…`, then ends on the same counter and view as the same search in one
  slice.

## Test

`test_terminal_search_scan.cpp`. Each invariant is an assertion on the match
total. INV-1 to INV-6 pinned behaviour the 2026-09-12 scan speed-up had to
keep, and were proven able to fail by mutating the scan. INV-5 and INV-7 as
written failed before the 2026-10-13 change to skip trailing blanks.
