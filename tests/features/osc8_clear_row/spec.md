# Feature: clearing text removes the links on it

## Invariants

**INV-1 — erasing a whole line removes its links.** A row holding an OSC 8
link, then erased with `CSI 2 K`, reports no hyperlink span.

**INV-2 — erasing the screen removes every row's links.** Links on two rows,
then `CSI 2 J`, leave no span on either row.

**INV-3 — a partial erase keeps a link outside the erased columns.** A link in
columns 0–3, then `CSI K` from column 10, still reports its span.

**INV-4 — a partial erase removes a link inside the erased columns.** A link
starting at column 10, then `CSI 1 K` from column 20, reports no span on that
row.

**INV-5 — printing over a link removes it.** A link in columns 0–3, then
plain `XXXX` printed from column 0, reports no span for that link.

**INV-6 — redrawing a link in place keeps one span.** The same link printed
200 times at the same position leaves exactly one span on its row.

**INV-7 — an empty link adds no span.** A link opened and closed with no text
between, 200 times, leaves no span on its row.

**INV-8 — a link that wraps off the bottom row keeps its span.** A link opened
five columns from the end of the last row, with a ten-character label, wraps
and scrolls the screen up one row before it closes. Its first five characters
report a span on the second-to-last row, columns 75–79, and its last five a
span on the last row, columns 0–4.

**INV-9 — an open link's start moves with a reverse scroll.** A link opened
at row 5, column 0, with the text `abcde`, then CSI T (scroll down one). The
cursor is moved to row 6, column 5 and the link closed. Row 6 has a span on
columns 0–4; row 5 has none.

**INV-10 — an open link's start moves with an inserted line.** As INV-9, but
CSI L at row 3 moves the text to row 6.

**INV-11 — an open link's start moves with a deleted line.** As INV-9, but
CSI M at row 3 moves the text to row 4; the link closes at row 4, column 5.
Row 4 has a span on columns 0–4; row 5 has none.

**INV-12 — a start pushed off the bottom restarts at the cursor.** A link
opened at the last row, column 0, with the text `abcde`, then CSI T; then
`xyz` and the close. The last row's span starts at column 5, where `xyz`
begins, not on the blank columns before it.

## Rationale

`TerminalGrid::clearRow` reset cells and combining characters but left the
row's `m_screenHyperlinks` entries in place. After a clear, the old link
stayed clickable over blank cells, and in-place redraws accumulated spans
without bound.

## Test surface

`test_osc8_clear_row.cpp` feeds byte strings through `VtParser` into a
`TerminalGrid` and reads `screenHyperlinks(row)`. No GUI.

## Regression history

- **ANTS-5076:** `clearRow` never cleared OSC 8 spans, so cleared text kept
  its links. Locked by this spec.
- **ANTS-5076 (INV-8):** `scrollUp` moved rows and their spans but not the open
  link's start row, so a link that wrapped off the bottom row closed as a
  single-row span that ended before it began and was dropped.
- **ANTS-5218 (INV-9 to INV-12):** `scrollDown`, `insertLines` and
  `deleteLines` moved rows under an open link without moving its start row,
  so the link's span landed on the row its text had left.
