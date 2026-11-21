<!-- ants-spec-format: 1 -->
# ANTS-3456 — Re-wrap the scrollback once per window drag, not once per step

**Status:** accepted (2026-11-21). No review gate: not a contract, cheap to reverse (rule 14).
**Kind:** perf.
**Source:** ROADMAP.md ANTS-3456 (user-request-2026-07-09).
**Pairs with:** ANTS-1781.

**Layman:** Dragging the window edge with a lot of scrollback no longer
freezes over and over; the text re-wraps once when you let go.

## 1. Problem

`TerminalWidget::resizeEvent` calls `TerminalWidget::recalcGridSize` on
every resize event. When the width changes, that calls
`TerminalGrid::resize`, which re-wraps the whole scrollback on the GUI
thread.

1. One re-wrap of a full 50,000-line scrollback (the default) takes 131 ms
   (`./build/bench_resize_reflow`, case `scrollback_50k`, 2026-11-21). The
   cost grows with the scrollback, whose maximum is 1,000,000 lines.
2. A drag sends a stream of resize events, and nothing coalesces them. So
   the window freezes once per step for as long as the drag lasts.
3. Each step also changes the shell's window size, so the shell redraws its
   prompt once per step.

After this ships, a drag re-wraps the scrollback at most twice: once at the
first step and once after the drag settles.

## 2. Surface

### 2.1 Settle timer in `resizeEvent`

`TerminalWidget` gains a single-shot `QTimer m_resizeSettle`
(`kResizeSettleMs = 150`) and a `bool m_resizePending`. The widget's
existing `m_searchDebounce` timer is the pattern.

```cpp
void TerminalWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    m_spanCacheDirty = true;
    if (m_resizeSettle.isActive()) {
        m_resizePending = true;           // trailing edge: apply on timeout
        positionScrollBar();
    } else {
        recalcGridSize();                 // leading edge: apply now
    }
    m_resizeSettle.start();               // (re)start the settle window
    updateScrollToBottomButton();
}

// timeout:
//   if (m_resizePending) { m_resizePending = false; recalcGridSize(); }
```

- **Leading edge.** The first event after a quiet period applies at once. A
  single resize, such as a maximise, a tiling snap or
  `MainWindow::resizeToCells`, behaves as today.
- **Trailing edge.** Events inside the window only restart the timer. When
  events stop for `kResizeSettleMs`, the last size is applied once.
- **Only `resizeEvent` defers.** Every other caller of `recalcGridSize`
  still applies at once: `setFontSize`, `setFontFamily`, `setPadding`,
  `showSearchBar`, `hideSearchBar`, `startShell` and `forceRecalcSize`.
  `recalcGridSize` does nothing when the size is unchanged, so a deferred
  apply after one of these is harmless.
- **The scroll bar and the scroll-to-bottom button follow every event.**
  They sit at the widget's edge, so a deferred grid must not strand them.
- **The PTY window size moves with the grid.** It is still set inside
  `recalcGridSize`, through the blocking call that
  `tests/features/threaded_resize_synchronous` pins. The grid and the shell
  never disagree, and the shell gets one size change per drag instead of
  one per step.

While a size is pending, the widget paints the grid at its old size. A wider
window shows blank space on the right. A narrower one clips the rightmost
columns until the drag settles.

### 2.2 Failure modes

- **A drag that never pauses for `kResizeSettleMs`.** The grid keeps the
  first step's size until the drag ends. This is the intended trade (§ 2.3).
- **The widget is destroyed while a size is pending.** The timer is a
  member, so it dies with the widget and nothing fires.
- **A hidden tab.** `recalcGridSize` already skips a widget that is not yet
  laid out. That check runs unchanged on both edges.
- **An export or a sync-output block during a drag.** Both are handled
  inside `recalcGridSize`: ANTS-5078 ends the export, ANTS-1199 re-takes the
  snapshot. They now run once per applied size instead of once per step.
- **A very large scrollback.** Each of the two re-wraps still freezes for
  as long as one re-wrap takes. This spec bounds how many re-wraps a drag
  costs, not how long one takes (§ 4).

### 2.3 Alternatives rejected

- **Trailing edge only.** One re-wrap per drag instead of two. Rejected
  because every single resize, such as a maximise, would lag by
  `kResizeSettleMs`. Code that resizes and then reads the grid at once
  would also need a flush.
- **Throttle: apply at most every N ms during the drag.** The text would
  follow the drag live. Rejected because each apply still freezes for one
  full re-wrap, which is the freeze this item removes.
- **Re-wrap off the GUI thread.** This would bound one re-wrap's cost as
  well as their number. Rejected for this item: the scrollback is read by
  paint, search, selection, link spans and the parse worker, so a
  concurrent re-wrap needs its own design.
- **Re-wrap only the visible part, the rest later.** Rejected for the same
  reason. Every scrollback index (search matches, prompt marks, link spans,
  export position) assumes the whole scrollback has one width.

## 3. Invariants

- **INV-1** — Resize events that arrive inside one settle window change the
  grid at most once more, after the window closes. Until then the grid keeps
  the size the first event gave it.
  *Test:* `tests/features/resize_reflow_coalesced/`: a shown widget gets ten
  resizes of different widths with no wait between them. After each one,
  the grid's `cols()` equals the first width's columns.
  *Breaks when:* `resizeEvent` calls `recalcGridSize` on every event, as it
  does today. The grid's columns then follow each resize.

- **INV-2** — Once the settle window closes, the grid matches the widget's
  last size. No final resize is lost.
  *Test:* `tests/features/resize_reflow_coalesced/`: after waiting past
  `kResizeSettleMs`, the grid's `cols()` and `rows()` equal the last size's
  columns and rows.
  *Breaks when:* the timeout handler skips the apply, or a restart of the
  timer clears `m_resizePending`.

- **INV-3** — Callers other than `resizeEvent` apply the new size at once,
  even inside a settle window.
  *Test:* `tests/features/resize_reflow_coalesced/`: inside a settle window,
  `setFontSize` with a new size leaves the grid at the size the widget's
  current geometry gives at the new cell size, with no wait.
  *Breaks when:* the deferral is put inside `recalcGridSize` instead of
  `resizeEvent`.

- **INV-4** — The scroll bar sits at the widget's right edge after every
  resize event, including a deferred one.
  *Test:* `tests/features/resize_reflow_coalesced/`: after a deferred
  resize, the scroll bar's `geometry().right()` equals `width() - 1`.
  *Breaks when:* `positionScrollBar` runs only inside `recalcGridSize`, so a
  deferred event leaves the scroll bar at the old edge.

## 4. Out of scope

- Making one re-wrap of a very large scrollback cheaper, or moving it off
  the GUI thread — deferred; not yet queued.
- The blocking call to the parse worker on resize — tracked by ANTS-1781.

## 5. Tests

Feature test: `tests/features/resize_reflow_coalesced/`, with its own
`spec.md`. Covers INV-1, INV-2, INV-3 and INV-4. Label `features`. It drives
an offscreen `TerminalWidget`, as `tests/features/window_cell_size` does.
Its bundle comes from `build_target_for`.

INV-1 and INV-4 must be seen failing against the pre-fix `resizeEvent`.
INV-2 and INV-3 pass before the fix and guard the fix's own mistakes.

`tests/perf/bench_resize_reflow.cpp` calls `TerminalGrid::resize` directly,
so its figure does not change. It stays the measure of one re-wrap's cost.

## 6. Reload

Nothing reloads. The change is widget code inside the terminal process, so
it reaches a running terminal on the next launch. It holds no state beyond
one timer and adds no config key, so it needs no stop/start control.

## 7. Cross-doc impact

- `CHANGELOG.md` `[Unreleased]`: one bullet saying what shipped.

## What checks this

| Rule | What catches a breach |
|---|---|
| INV-1 | `tests/features/resize_reflow_coalesced/` |
| INV-2 | `tests/features/resize_reflow_coalesced/` |
| INV-3 | `tests/features/resize_reflow_coalesced/` |
| INV-4 | `tests/features/resize_reflow_coalesced/` |

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
