# Tab bar + status bar opacity under translucent parent

## Problem

Same translucent-parent failure mode as `menubar_hover_stylesheet`,
caught in a different bar. `MainWindow` sets `Qt::WA_TranslucentBackground`
on itself for the per-pixel terminal-area opacity feature. That
attribute disables auto-fill for the entire widget tree, so any child
widget relying on palette-based or QSS-based background fill renders
over a cleared-to-transparent surface unless it actively paints
something opaque itself.

The menubar bug was fixed in 0.7.26 via the `OpaqueMenuBar` subclass
whose `paintEvent` calls `QPainter::fillRect(rect(), m_bg)` with
`CompositionMode_Source` before delegating to the base class. That fix
was scoped to the menubar — the same root cause continued to bite the
tab bar's empty area (right of the last tab) and the entire status
bar background.

User report 2026-04-25 (post-0.7.32):
> "Both the backgrounds for the menubar and the status bar are now
> completely transparent."
> [follow-up] "The menubar is fine, it the background of the tab bar.
> The tabs themselves are fine but the rest of the tab bar across the
> window is transparent as well as the background for the status bar."

The 0.7.32 close-button SVG stylesheet changes drew the user's eye to
the empty-area fill that had been mis-painted all along; the bug was
not introduced by 0.7.32 but became visible because the new tab look
re-balanced what the user noticed.

## Fix (0.7.36)

Mirror the `OpaqueMenuBar` pattern:

1. `ColoredTabBar` gains `setBackgroundFill(QColor)` and prepends a
   `CompositionMode_Source` `fillRect` to its existing `paintEvent`,
   before the base class draws the tabs and before the colour-group
   gradient overlay.
2. New header-only `OpaqueStatusBar` (mirror of `OpaqueMenuBar`) —
   `paintEvent` calls `fillRect` then delegates to `QStatusBar`.
3. `MainWindow` constructs an `OpaqueStatusBar`, installs it via
   `setStatusBar(...)` *before* the first `statusBar()` call, sets
   `WA_OpaquePaintEvent` + `WA_StyledBackground` + `autoFillBackground`
   on both the tab bar and the new status bar, and pushes
   `theme.bgSecondary` into both via `setBackgroundFill` from
   `applyTheme()`.

## Invariants

This test is source-grep based (no Qt link, no display required) —
same harness model as `menubar_hover_stylesheet`. Each invariant pins
one piece of the fix that, if reverted, re-opens the bug.

- **INV-1** (tab bar opaque-paint contract):
  `ColoredTabBar` exposes `setBackgroundFill(...)` and its
  `paintEvent` calls `fillRect` (the actual opaque-paint mechanism).
- **INV-2** (tab bar wired):
  `MainWindow` sets `WA_OpaquePaintEvent` on `m_coloredTabBar` at
  construction and calls `m_coloredTabBar->setBackgroundFill(...)`
  from `applyTheme`. Without the fill call, the override has no
  colour and renders transparent.
- **INV-3** (status bar opaque-paint contract):
  `src/opaquestatusbar.h` declares an `OpaqueStatusBar` whose
  `paintEvent` calls `fillRect`. A no-op subclass would pass the
  construction-site checks below but still show the desktop through
  the bar.
- **INV-4** (status bar wired):
  `MainWindow` constructs `m_statusBar` as `new OpaqueStatusBar(...)`
  and installs it via `setStatusBar(m_statusBar)` BEFORE the first
  `statusBar()` call (Qt lazy-creates a plain `QStatusBar` on first
  access otherwise). `applyTheme` calls
  `m_statusBar->setBackgroundFill(...)`.
- **INV-5** (paint order in tab bar):
  `ColoredTabBar::paintEvent` performs the `fillRect` BEFORE calling
  `QTabBar::paintEvent` — otherwise the base class draws first and
  the fill paints over the tabs.

## How to verify pre-fix code fails

```bash
git checkout 07a9976 -- src/coloredtabbar.cpp src/coloredtabbar.h \
                        src/mainwindow.cpp src/mainwindow.h
rm src/opaquestatusbar.h
cmake --build build --target test_tabbar_statusbar_opaque
ctest --test-dir build -R tabbar_statusbar_opaque
# Expect: failures on INV-1 through INV-5.
```

Then `git checkout HEAD -- src/coloredtabbar.* src/mainwindow.* src/opaquestatusbar.h`
to restore, rebuild, and confirm the test passes.
