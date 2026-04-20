# TerminalWidget PartialUpdate — dropdown-flicker invariant

## Problem

Users reported the File / Edit / View / Settings dropdown menus
flickering whenever the cursor moved over the owning menubar item
while the dropdown was open. Six attempted fixes via stylesheet
tweaks (`::item:hover`, `::item` base rule, removing `border-radius`),
menubar attributes (`autoFillBackground`, `WA_StyledBackground`,
`WA_OpaquePaintEvent`, `setUpdatesEnabled(false)` while menu open),
per-menu attributes (`WA_TranslucentBackground=false`,
`WA_NoSystemBackground`, `WA_OpaquePaintEvent`, `autoFillBackground`,
`setMouseTracking(false)`), and event-filter swallowing of
intra-action mouse-moves all either partially reduced the flicker or
had zero effect. Crucially, disabling `WA_TranslucentBackground` on
the MainWindow via a diagnostic env var also did not fix it.

## Root cause

`ANTS_PAINT_LOG=1` instrumentation revealed the true cause:
`MainWindow` was receiving ~60 Hz `UpdateRequest` events, each
triggering a full cascade of paint events across every child widget
(TitleBar, QMenuBar, ColoredTabWidget, QStackedWidget, TerminalWidget,
QStatusBar). The trigger was the `TerminalWidget` (a
`QOpenGLWidget`) in its default `QOpenGLWidget::NoPartialUpdate`
mode.

In `NoPartialUpdate` mode, every GL-widget paint invalidates the
entire top-level window so the compositor can blend the GL FBO with
the rest of the window in one pass. That's expensive but correct when
the GL widget is translucent. When the shell output animates
anything — Claude Code's spinner, the cursor blink, OSC 9;4 progress,
etc. — the terminal repaints. Each repaint cascades to every child
including the menubar, which repaints while the dropdown popup is
overlaid, and the popup visibly flickers as the damaged compositor
region beneath it reblends.

## Fix

`src/terminalwidget.cpp::TerminalWidget::TerminalWidget()` calls
`setUpdateBehavior(QOpenGLWidget::PartialUpdate)`. In this mode, the
GL widget composites itself without invalidating the top-level
window; sibling widgets (QMenuBar, dropdown popup, etc.) are never
repainted as a side-effect of terminal output.

## Invariants

Source-grep, because the flicker reproduction needs an animated shell
workload + live compositor, neither of which CI has.

- **INV-1:** `src/terminalwidget.cpp`'s `TerminalWidget` constructor
  calls `setUpdateBehavior(QOpenGLWidget::PartialUpdate)`.
- **INV-2 (neg):** it does NOT set `NoPartialUpdate` or pass no
  argument (which would select the NoPartialUpdate default).
