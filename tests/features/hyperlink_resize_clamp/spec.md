# Feature contract — active OSC 8 hyperlink clamps across resize

## Motivation

When an OSC 8 hyperlink is opened but not yet closed (the app has
emitted `OSC 8 ;; url ST` but not the matching `OSC 8 ;; ST`),
`TerminalGrid` holds the starting `(row, col)` coordinate pair in
`m_hyperlinkStartRow` / `m_hyperlinkStartCol`. The span is only
committed to `m_screenHyperlinks` once the close sequence arrives.

If a resize arrives in the open-but-uncommitted window, the stored
start coordinates may now point at a row / column that no longer
exists. Without a clamp, a subsequent grow-back could attach the
span to a row whose content no longer matches the clickable text.
The 0.7.7 fix added `std::clamp` on both fields inside
`TerminalGrid::resize`, matching the existing clamps on cursor
row/col.

## Invariants

**I1 — After a shrink resize, the active-hyperlink start row does
not exceed the new maximum row index.** Equivalent: the next span
commit lands on a valid row, not at the pre-resize (now-out-of-
range) row.

**I2 — After a shrink resize, the active-hyperlink start column
does not exceed the new maximum column index.**

**I3 — The clamp does not throw or crash when the start coordinates
were legal pre-resize.** The clamp runs on every resize; it must
tolerate no-op cases.

**I4 — A subsequent OSC 8 close after shrink resize commits the
span on a valid row, not at the stale pre-resize row.**

## Scope

In scope: runtime exercise of `TerminalGrid::resize` on a grid
holding an open OSC 8 hyperlink. Observation is indirect: the test
closes the hyperlink after resize and asserts the committed span
appears on a valid row (0..m_rows-1).

Out of scope:
- The case where the hyperlink was already committed before resize
  — that span lives in `m_screenHyperlinks`, which `resize()`
  already handles by `.resize(m_rows)`.
- Widening resizes — clamps on grow are no-ops by definition.

## Test execution

`test_hyperlink_resize_clamp.cpp`:

1. Build a 24×80 grid with VtParser.
2. Move cursor to (20, 50) and open an OSC 8 hyperlink (the half
   without the closing `OSC 8 ;; ST`).
3. Call `resize(5, 10)` — shrink that makes (20, 50) invalid.
4. Close the hyperlink with `OSC 8 ;; ST`.
5. Scan every valid row in the resized grid; assert the link's URI
   appears on exactly one row whose index is in `[0, 4]` and whose
   column value is in `[0, 9]`. Any row outside that range, or a
   throw during any of the above steps, is a failure.

Exit 0 on success; non-zero with the violating row/col printed
otherwise.
