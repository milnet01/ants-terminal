# Resize reflow coalesced

**Status:** locks ANTS-3456. Contract:
[`docs/specs/ANTS-3456-resize-reflow-coalescing.md`](../../../docs/specs/ANTS-3456-resize-reflow-coalescing.md).

## What we test

An offscreen `TerminalWidget`, a child of a shown host so each `resize()`
sends its resize event at once.

1. **INV-1.** Ten resizes of different widths, with no wait between them,
   leave the grid at the width the first one gave it.
2. **INV-2.** After the settle window closes, the grid matches the widget's
   last size: `forceRecalcSize` then changes nothing.
3. **INV-3.** Inside a settle window, `setFontSize` applies at once:
   `forceRecalcSize` straight after it changes nothing.
4. **INV-4.** After a deferred resize, the scroll bar's right edge is the
   widget's right edge.

INV-1 fails against the pre-fix `resizeEvent`, which re-sized the grid on
every event. INV-2 to INV-4 pass before the fix and guard the fix itself.
