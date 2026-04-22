# Feature contract — OSC 8 hyperlinks track insertLines / deleteLines

## Motivation

OSC 8 hyperlinks are stored in a per-screen-row vector
(`m_screenHyperlinks`) that parallels the cell vector
(`m_screenLines`). CSI L (Insert Line, `insertLines`) and CSI M
(Delete Line, `deleteLines`) rearrange rows in the main screen
buffer to implement scrolling within a margin — vim uses them
heavily during `:insert`/`:delete`, as do any editors that edit a
middle region of the screen.

Before the 0.7.7 fix, `insertLines`/`deleteLines` manipulated
`m_screenLines` but left `m_screenHyperlinks` untouched. A clickable
link on row 5 would drift to row 4 (or 6) after a single Insert-Line
at row 3 — the underline visual moved with the text, but the
clickable bounding box stayed behind, so clicking the visible
hyperlink did nothing or opened the wrong URL. The fix: run the
same erase/insert pattern against `m_screenHyperlinks` in lock-step,
gated on `bottom < m_screenHyperlinks.size()` so the lazy-grow path
at `addRowHyperlink` doesn't throw when the table is under-populated.

## Invariants

**I1 — CSI L (Insert Line) shifts hyperlinks downward in parallel
with cells.** If an OSC 8 span is committed at row `r` and
`insertLines(n)` runs at cursor row `c <= r`, the span afterwards
must be at row `r + n` (or out of the scroll region, in which case
it is dropped by the same rules applied to cells).

**I2 — CSI M (Delete Line) shifts hyperlinks upward in parallel
with cells.** Symmetric to I1.

**I3 — A span on a row inside the scroll region but outside the
affected range stays put.** Only rows in the [cursorRow, bottom]
window are touched.

**I4 — Operations do not throw on under-populated hyperlink
tables.** The `hlInRange` guard in both functions keeps lazy-grow
paths safe.

## Scope

In scope: direct runtime exercise of `TerminalGrid::insertLines`
and `TerminalGrid::deleteLines` via CSI L / CSI M escape sequences,
with OSC 8 hyperlinks previously committed on specific rows.
Observations use the public `screenHyperlinks(row)` accessor.

Out of scope:
- Scrollback hyperlinks — insertLines/deleteLines only touch the
  main screen buffer.
- Alt-screen hyperlinks — the 0.7.7 fix also touched alt-screen
  paths (covered by I1/I2 implicitly since the same code is reused).
- OSC 8 parsing correctness (covered elsewhere).
- Click-path routing from pixel to span (separate subsystem).

## Test execution

`test_osc8_linerange.cpp` builds a `TerminalGrid` + `VtParser`
harness, then:

1. Writes an OSC 8 link at row 5 (`OSC 8 ;; url ST text OSC 8 ;; ST`).
   Asserts the span lands at row 5 via `screenHyperlinks(5)`.
2. Moves cursor to row 2, issues `CSI 2 L` (insert 2 lines).
   Asserts `screenHyperlinks(5)` is empty and `screenHyperlinks(7)`
   now carries the original span. This is I1.
3. Repeats on a fresh harness: writes span at row 5, cursor to row
   2, `CSI 2 M` (delete 2 lines). Asserts span moves to row 3.
   This is I2.
4. Writes span at row 0 (above scroll region after cursor-at-10
   setup). Issues `CSI 1 L` from cursor 10. Asserts span at row 0
   unchanged. This is I3.

Exit 0 on all four invariants holding; non-zero with the failing
invariant printed otherwise.
