# Search matches follow their lines when scrollback drops its oldest

`TerminalWidget::performSearch` stores each match by global line: the line's
index counting from the oldest scrollback line. Once scrollback is at its
cap, every line pushed into it drops the oldest one, and every line behind
it takes the next lower index. `TerminalWidget::onVtBatch` measures how many
lines left the front of scrollback while it applied a batch, and moves the
stored matches by that many (ANTS-5686). Before, the stored numbers stayed
put and the highlights painted on other lines.

New output is not searched until the next search runs.

## Invariants

- **INV-1 — a match moves with its line.** With scrollback at its cap and a
  match on a scrollback line, a batch that pushes more lines leaves the
  match on the line that holds the text, and none on its old index.
- **INV-2 — a match whose line was dropped is removed.** A match on a line
  the batch pushes out of scrollback is gone, and the counter label reads
  the number left.

## Test

`test_search_matches_follow_scrollback.cpp` in the `test_chrome` bundle. A
real widget takes its lines through the real parser, the last of them as a
`VtBatch` handed to `onVtBatch`. The expected line is found by reading the
grid for the cells that spell the needle. Both tests fail against the code
before the change.
