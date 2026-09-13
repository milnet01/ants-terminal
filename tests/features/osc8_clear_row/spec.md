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
