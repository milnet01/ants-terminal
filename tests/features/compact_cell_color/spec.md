# compact_cell_color — a cell stores its colours in 4 bytes each

**Bundle:** `test_vt` · **Suite:** `CompactCellColor` · **Label:** `features;fast`

## Problem

Every character on screen and in scrollback is a `Cell`. Its three
`QColor` members took 48 of its 64 bytes, and cell storage is 99% of a full
scrollback's heap (`tests/perf/bench_scrollback_memory`, 2026-11-06: 614 MiB
per tab at the 50,000-line default and 200 columns). ANTS-6702 stores each
colour as one 32-bit RGBA value.

## Invariants

- **INV-1** — `sizeof(Cell)` is at most 28 bytes.
- **INV-2** — A colour written to a cell reads back with the same RGBA
  value, opaque or translucent.
- **INV-3** — A cell colour that was never set, or was set to an invalid
  `QColor`, reads back invalid, so "use the default colour" survives.
- **INV-4** — Colours set through the VT parser (`SGR 38;2`, `48;2`, `58;2`)
  land in the cell with their RGB values, and SGR 0 clears them to invalid.

## Out of scope

Channel precision beyond 8 bits per channel: terminal colours are 8-bit.
Saved sessions already store `rgba()`, so their format does not change.
