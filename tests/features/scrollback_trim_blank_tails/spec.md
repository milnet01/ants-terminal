# scrollback_trim_blank_tails — scrollback rows drop their blank tails

**Bundle:** `test_vt` · **Suite:** `ScrollbackTrimBlankTails` · **Label:** `features;fast`

## Problem

Every scrollback row kept the full screen width, so a five-character line
cost as much as a full one. Only 38% of stored cells came before a row's
last non-blank one (`tests/perf/bench_scrollback_memory`, 2026-11-06).
ANTS-6703 stores a row without its blank tail.

## Invariants

- **INV-1** — A row that ends a line (not soft-wrapped) enters scrollback
  holding cells only up to its last cell that is not a plain blank. A plain
  blank is a space, not part of a wide character, with no underline,
  inverse or strikethrough, no combining mark, and a background that is
  unset or the default.
- **INV-2** — Trailing spaces that show something are kept: a coloured
  background (for example after `ESC[44m ESC[K`), an underline, inverse
  video or strikethrough.
- **INV-3** — A soft-wrapped row is stored whole, so reflow joins it to the
  next row without losing a space at the wrap point.
- **INV-4** — A resize keeps scrollback rows trimmed, and reflow's text is
  unchanged: a line split across a wrap at a space rejoins with the space.
- **INV-5** — The full-width buffer a trimmed row leaves behind returns to
  the row pool, so scrolling does not allocate a new screen row each line.

INV-1 to INV-4 drive a `TerminalGrid` through the VT parser and read
`scrollbackLine()`. INV-5 reads the push path's source.
