# Feature contract — active OSC 8 hyperlink reset on resize

## Motivation

When an OSC 8 hyperlink is opened but not yet closed (the app has
emitted `OSC 8 ;; url ST` but not the matching `OSC 8 ;; ST`),
`TerminalGrid` holds the starting `(row, col)` coordinate pair in
`m_hyperlinkStartRow` / `m_hyperlinkStartCol`. The span is only
committed to `m_screenHyperlinks` once the close sequence arrives.

If a resize arrives in the open-but-uncommitted window, the stored
start coordinates may now point at a row / column that no longer
exists.

**History.** The 0.7.7 fix attempted to handle this with a `std::clamp`
on both fields inside `TerminalGrid::resize` — but the clamp itself
reintroduced the stale-coordinate bug: after a shrink that dropped
`m_hyperlinkStartRow` into scrollback, the clamp pulled it up to a
different row on the new screen, so the next OSC 8 close emitted a
span over rows the user never marked. The accompanying comment
explicitly claimed "Close the active hyperlink on any resize — matches
xterm's behaviour" — but the code only clamped.

**Indie-review-2026-05-14 lane-1 H2 fix.** The implementation now does
what the comment always promised: a resize closes the in-progress
hyperlink (resets `m_hyperlinkActive=false`, clears `m_hyperlinkUri`
/ `m_hyperlinkId`, zeroes the start coords). A subsequent OSC 8
close sequence is a no-op (there is nothing open to close); a fresh
OSC 8 open after resize works normally.

## Invariants

**I1 — A resize during an open OSC 8 sequence closes the hyperlink.**
After `resize(...)`, `m_hyperlinkActive == false`,
`m_hyperlinkUri.isEmpty()`, `m_hyperlinkId.isEmpty()`, and
`m_hyperlinkStartRow == m_hyperlinkStartCol == 0`. Observable via
`screenHyperlinks(r)` — no span gets committed for the open-but-
uncommitted URL when the subsequent close arrives after the resize.

**I2 — The reset does not throw or crash for any combination of
pre-resize coordinates and new dimensions.** Resize is called on
every window-size change; it must tolerate the open case as a
no-op-from-a-correctness-standpoint (the user-visible effect is the
hyperlink is closed silently — matches xterm).

**I3 — A fresh OSC 8 sequence after the resize works normally.**
The reset clears state but does not poison the parser; emitting a
new `OSC 8 ;; url ST` → text → `OSC 8 ;; ST` sequence post-resize
must commit a valid span on the new screen.

## Scope

In scope: runtime exercise of `TerminalGrid::resize` on a grid
holding an open OSC 8 hyperlink. Observation is indirect: the test
verifies the open hyperlink is dropped after resize, and that a
subsequent OSC 8 sequence post-resize commits cleanly.

Out of scope:
- The case where the hyperlink was already committed before resize
  — that span lives in `m_screenHyperlinks`, which `resize()`
  already handles by `.resize(m_rows)`.
- Widening resizes — same reset behaviour as shrinks; the test
  exercises the shrink path because it's the historically buggy one.

## Test execution

`test_hyperlink_resize_clamp.cpp`:

1. Build a 24×80 grid with VtParser.
2. Move cursor to (20, 50) and open an OSC 8 hyperlink (the half
   without the closing `OSC 8 ;; ST`).
3. Call `resize(5, 10)` — shrink that makes (20, 50) invalid.
4. Close the hyperlink with `OSC 8 ;; ST`.
5. Assert no span with the URL appears in the resized grid (the
   open hyperlink was reset by the resize; the close is a no-op).
6. Then emit a fresh `OSC 8 ;; url ST` → text → close sequence on
   row 2 of the new grid; assert the span lands on row 2.

Exit 0 on success; non-zero with the violating state printed
otherwise.
