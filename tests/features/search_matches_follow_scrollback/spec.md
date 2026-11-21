# Search matches follow their lines when scrollback drops its oldest

`TerminalWidget::performSearch` stores each match by global line: the line's
index counting from the oldest scrollback line. Once scrollback is at its
cap, every line pushed into it drops the oldest one, and every line behind
it takes the next lower index. `TerminalWidget::onVtBatch` measures how many
lines left the front of scrollback while it applied a batch, and moves the
stored matches by that many (ANTS-5686). Before, the stored numbers stayed
put and the highlights painted on other lines.

While a search is active, `onVtBatch` also re-matches every line from the
first one that was on screen before the batch to the end (ANTS-6767).
Scrollback above that line cannot change in a batch. The cost per batch is
the screen plus the lines the batch pushed, and nothing while no search is
active.

## Invariants

- **INV-1 — a match moves with its line.** With scrollback at its cap and a
  match on a scrollback line, a batch that pushes more lines leaves the
  match on the line that holds the text, and none on its old index.
- **INV-2 — a match whose line was dropped is removed.** A match on a line
  the batch pushes out of scrollback is gone, and the counter label reads
  the number left.
- **INV-3 — new output is matched.** A line the batch prints that holds
  the pattern carries a match, and the counter counts it.
- **INV-4 — a line rewritten in place loses its match.** When the batch
  erases a matched screen line and prints other text, the match is gone and
  the counter reads `0/0`.
- **INV-5 — the current match and the view stay.** New matches leave the
  counter's current number where it was, and the top line of the view does
  not move.
- **INV-6 — new output past the cap drops the oldest matches** (ANTS-6494).
  A search capped at 100000 matches, then more matching output, keeps the
  total at `100000+` and matches the newest line.
- **INV-7 — output during a running search** (ANTS-5418). A search scans in
  slices between event-loop turns. Output that drops lines off the front and
  prints new matches while it runs leaves it, once finished, matching exactly
  the lines that hold the pattern. This holds whether the output lands on
  lines the scan has reached or on lines it has not.

## Test

`test_search_matches_follow_scrollback.cpp` in the `test_chrome` bundle. A
real widget takes its lines through the real parser, the last of them as a
`VtBatch` handed to `onVtBatch`. The expected line is found by reading the
grid for the cells that spell the needle. Each test fails against the code
before the change it locks: INV-1 and INV-2 before ANTS-5686, INV-3 to
INV-5 before ANTS-6767, INV-6 before ANTS-6494, INV-7 before ANTS-5418.
