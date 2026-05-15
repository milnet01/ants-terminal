# scrollback_hyperlinks_reflow_lockstep — ANTS-1333

See `docs/specs/ANTS-1333.md` for the full contract.

## Test scope

Asserts that after `TerminalGrid::resize(rows, cols)` triggers
width-change scrollback reflow, `m_scrollbackHyperlinks.size() ==
m_scrollback.size()` and the per-row spans behave per
INV-2 / INV-3 / INV-4.

## Invariants checked

- **INV-1.** Lockstep length after in-place narrow / grow.
- **INV-2.** Span survives across in-place reflow (clipped if
  endCol exceeds new cols).
- **INV-3.** Slow-path rewrap emits empty spans.
- **INV-4.** Screen→scrollback overflow push emits empty spans.
- **INV-5.** Cap-trim pops from both deques together.

## Repro before the fix

`m_scrollbackHyperlinks` is not touched in the reflow loop at
`terminalgrid.cpp:2458–2528`. After resizing a grid with N
scrollback rows + N hyperlink entries to a different width, if the
new scrollback has N' != N rows, the two deques drift. The test
fails on `scrollbackSize() == scrollbackHyperlinksCount()`.
