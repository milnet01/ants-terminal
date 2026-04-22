# Scroll-region rotate invariant

## Why this test exists

`TerminalGrid::scrollUp(count)` / `scrollDown(count)` move rows inside
`[scrollTop, scrollBottom]` and blank the vacated rows. The shipped
implementation used an `erase/insert` pair inside a `for (i < count)`
loop — each iteration O(rows). 0.7.9 replaces that loop with a single
`std::rotate` over the region. This test pins the observable behavior
so the swap can't silently regress.

Applies equally to either implementation — the test is phrased as
invariants over `scrollUp` / `scrollDown`, not as a check that any
particular algorithm is in use.

## Invariants

### I1 — `scrollUp(count)` shifts rows up inside the region.
After calling `scrollUp(count)` with `1 ≤ count ≤ regionHeight`:
- The row that was at `scrollTop + count + i` is now at `scrollTop + i`,
  for `i ∈ [0, regionHeight - count - 1]` — content-equal, cell-for-cell.
- The bottom `count` rows of the region are blank (default fg/bg, space).

### I2 — `scrollDown(count)` shifts rows down inside the region.
After calling `scrollDown(count)` with `1 ≤ count ≤ regionHeight`:
- The row that was at `scrollTop + i` is now at `scrollTop + count + i`,
  for `i ∈ [0, regionHeight - count - 1]`.
- The top `count` rows of the region are blank.

### I3 — Rows outside the region are untouched.
For any row `r ∉ [scrollTop, scrollBottom]`, the content at `r` before
the call equals the content at `r` after the call. (Applies to both
`scrollUp` and `scrollDown`.)

### I4 — Main-screen `scrollUp` with `scrollTop == 0` pushes to scrollback.
When the main screen is active and `scrollTop == 0`, `scrollUp(count)`
must push exactly `count` rows into scrollback (modulo the CSI 2J
doubling-guard window, which this test does not exercise). Pushed rows
are the old top `count` rows, in order.

### I5 — `scrollUp` with `scrollTop > 0` does NOT push to scrollback.
A scroll region that doesn't include row 0 is a TUI scroll (e.g. vim
paged status area); it must not leak rows into the user's scrollback.

### I6 — Alt screen `scrollUp` does NOT push to scrollback.
Even at `scrollTop == 0`, if the alt screen is active, the scroll
must not push into scrollback. (Alt-screen apps own the whole grid,
don't contribute to history.)

### I7 — Hyperlinks follow their row.
`m_screenHyperlinks[r]` tracks the row at index `r`. After a scroll,
each hyperlink span must still be on the same cells it was on before
the scroll (now at a different row index). Vacated rows' hyperlink
vectors are empty.

### I8 — `count > regionHeight` is clamped.
Calling `scrollUp(regionHeight + N)` or `scrollDown(regionHeight + N)`
for `N ≥ 1` is equivalent to calling it with `count = regionHeight`:
the whole region goes blank; no extra blank rows leak into scrollback.

## Test shape

Feed unique text into each row via the parser (so each row is
identifiable by its content), then call `scrollUp` / `scrollDown`
directly on the grid and assert the invariants. Rows are compared
by the decoded cell codepoints, not by raw cell struct equality —
keeps the test tolerant of incidental attribute/metadata churn.
