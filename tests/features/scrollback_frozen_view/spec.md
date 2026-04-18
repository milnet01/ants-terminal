# Feature: Frozen-view snapshot on scroll-up

## Contract

When the user is scrolled up in history
(`TerminalWidget::m_scrollOffset > 0`), **the visible viewport content
MUST NOT change** in response to any PTY output — including output
that uses cursor positioning (`CSI Pn;Pn H`) or direct erase
sequences (`CSI K`, `CSI J`) to overwrite cells on the live screen.

Formally, for any snapshot of the viewport taken at scroll-up time:

    snap_t0 = viewport_snapshot_when_scrolloffset_went_from_0_to_positive
    <any amount of PTY output>
    snap_t1 = viewport_snapshot_now
    ⟹ snap_t0 == snap_t1   for every row of the viewport.

The invariant holds **including the partial-overlap zone** — viewport
rows that would map to screen rows on the live grid. This is the
critical upgrade over the 0.6.25 spec, which only guaranteed stability
for rows fully in scrollback.

Returning to the bottom (`m_scrollOffset == 0`) resumes live rendering
and discards the snapshot.

## Rationale

User feedback 2026-04-18 (the third round of scrollback complaints):

> I wanted the ability to be able to scrollback and read previously
> provided information while Claude Code was adding to the viewport at
> the bottom. That is basically the whole feature request. What first
> started happening is that the whole window's text would redraw at
> the line I was reading almost duplicating the entire scrollable
> text of the window. Then after you implemented a fix for that, what
> happens now is that it starts overwriting at the line that I am
> reading at.

The 0.6.21–0.6.25 fixes layered pause/anchor logic to stabilize the
scrollback portion of the viewport, but the partial-overlap zone was
documented as "partial guarantee" — *inherent terminal semantics,
the library cannot override.* The user rejects that framing: from
their perspective, "I scrolled up to read" means the content stays
put, period.

0.6.33 addresses this by capturing a **frozen snapshot** of the live
screen at the moment the user scrolls up. While scrolled,
`cellAtGlobal()` and `combiningAt()` read screen-row content from the
snapshot instead of the live grid. Incoming PTY output still mutates
the live grid — it just isn't visible until the user returns to the
bottom. The frame the user was reading is truly frozen.

## Scope

### In scope
- Cursor-positioned overwrites (`CSI Pn;Pn H<text>`) during a
  scrolled-up viewport with a frozen snapshot.
- Line-feed-induced screen scrolls during a scrolled-up viewport — the
  existing anchor logic (0.6.25) still handles these, and the snapshot
  composes with it (when a scroll happens, scrollback grows, offset
  advances by the same delta; snapshot remains frozen).
- Erase-display (`CSI 2J`) during a scrolled-up viewport — the snapshot
  hides the clear from the user's view.
- Deep-scroll case (viewport entirely in scrollback) continues to work
  unchanged; the snapshot is harmless there.

### Out of scope
- The live grid itself — it still receives all writes. Programs that
  query cell content (rare) see live state, not snapshot state.
- Resize while scrolled-up — the snapshot is invalidated on resize
  because its row/col dimensions no longer match. The viewport reverts
  to live rendering for the remainder of the scrolled-up session.
  Tested separately in `combining_on_resize`.
- Alt-screen entries during scrolled-up state — `recalcGridSize` and
  the alt-screen swap don't currently interact with the snapshot;
  entering alt screen while scrolled up is a rare path and falls back
  to live rendering without a specific guard.

## Regression history

- **0.6.20 and earlier:** scrollback doubled on every Claude Code
  TUI repaint. RAM grew unboundedly.
- **0.6.21:** pause-while-scrolled-up stopped the doubling but caused
  "line 251 becomes line 250" viewport shift because the push counter
  froze too.
- **0.6.22:** sliding-window suppression for `CSI 2J` repaint on the
  main screen. Composed with 0.6.21 pause.
- **0.6.25:** viewport-stable anchor — when user is scrolled up,
  always push to scrollback and advance `m_scrollOffset` by the push
  count. Locked the deep-scroll case.
- **0.6.33** (this spec): frozen-view snapshot. Locks the
  partial-overlap case that 0.6.25 documented as "partial guarantee."
  The snapshot discards on return-to-bottom and on resize.

## Test strategy

The snapshot lives in `TerminalWidget` (QOpenGLWidget subclass,
expensive to link). Instead of linking the full widget, this test
simulates the snapshot at the TerminalGrid level: capture a
`std::vector<std::vector<Cell>>` copy of the screen rows, then
verify that reading from that copy after arbitrary PTY output
yields the same cell content. This mirrors exactly what
`TerminalWidget::captureScreenSnapshot` /
`TerminalWidget::cellAtGlobal` do — the widget code is a lookup
table on the same vector shape.

The source-level side of the test also asserts that:
- `TerminalWidget::captureScreenSnapshot` is called from
  `updateScrollBar` on the 0 → >0 scroll-offset transition.
- `TerminalWidget::clearScreenSnapshot` is called on the >0 → 0
  transition and on resize.

Source-grep approach mirrors `review_changes_clickable` and
`shift_enter_bracketed_paste`.
