# TerminalWidget is a plain QWidget — dropdown-flicker invariant

> Legacy test name: `terminal_partial_update_mode`. Kept because
> removing it would rewrite CMake + CTest registration; the *meaning*
> has shifted away from partial-update mode (see "History" below).

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
had zero effect. Disabling `WA_TranslucentBackground` on the
MainWindow via a diagnostic env var also did not fix it.

## Root cause

`ANTS_PAINT_LOG=1` instrumentation revealed the true cause:
`MainWindow` was receiving ~60 Hz `UpdateRequest` events, each
triggering a full cascade of paint events across every child widget
(TitleBar, QMenuBar, ColoredTabWidget, QStackedWidget,
TerminalWidget, QStatusBar). The trigger was `TerminalWidget`
inheriting from `QOpenGLWidget`.

Qt6's `QOpenGLWidget` composes its FBO through the parent's backing
store on every paint — even in `PartialUpdate` mode, the top-level
window is re-blended so the GL widget's alpha can mix with the
surrounding chrome. When the shell output animates anything (Claude
Code's spinner, the cursor blink, OSC 9;4 progress, etc.), the
terminal repaints, the parent re-blends, and the menubar repaints
while the dropdown popup is overlaid. The popup visibly flickers as
the damaged compositor region beneath it reblends.

Calling `setUpdateBehavior(QOpenGLWidget::PartialUpdate)` was tried
(hence the original name of this test) and did not fix it — the
FBO-through-backing-store composition path is unconditional in Qt6
when the parent has `WA_TranslucentBackground`, regardless of update
mode.

## Fix

`TerminalWidget` no longer inherits from `QOpenGLWidget` at all.
`terminalwidget.h` declares
`class TerminalWidget : public QWidget` and the `.cpp` calls only
`QWidget::` base methods. `makeCurrent()` / `doneCurrent()` calls
were removed (no GL context); per-frame rendering uses the QPainter
path through `QWidget::paintEvent` on the regular backing store. GL
rendering capability moved to the optional `glrenderer` module,
which can be re-instantiated inside a container if someone revives
the GPU path (tracked in ROADMAP 0.7.12 Tier 3 "renderer subsystem
decision").

## Invariants

Source-grep, because reliable flicker reproduction needs an animated
shell workload + live compositor (not available in CI).

- **INV-1:** `src/terminalwidget.h` declares
  `class TerminalWidget : public QWidget`. Reverting to
  `QOpenGLWidget` re-opens the ~54 Hz full-window repaint cascade +
  dropdown-flicker regression.
- **INV-2 (neg):** `src/terminalwidget.h` does not `#include <QOpenGLWidget>`.
- **INV-3 (neg):** `src/terminalwidget.cpp` contains no
  `QOpenGLWidget::` call-shape (`QOpenGLWidget::<identifier>(...)`).
  Comments that mention `QOpenGLWidget::` are allowed — the history
  narrative above does exactly that.
- **INV-4 (neg):** `src/terminalwidget.cpp` contains no live
  `makeCurrent()` call as a statement. Comment references remain
  (they explain the refactor).

## History

- **0.6.x — 0.7.3:** `TerminalWidget : public QOpenGLWidget`.
  Dropdown-flicker reports accumulated across this entire range.
- **0.7.4:** `setUpdateBehavior(PartialUpdate)` tried — did not fix.
- **0.7.4 (later):** full base-class change to `QWidget`. Fix
  confirmed on KWin + Mutter. Test renamed invariants to match
  (the directory name is preserved for CMake stability).
