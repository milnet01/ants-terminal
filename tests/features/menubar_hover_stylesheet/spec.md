# Menubar opacity & hover-flicker invariants

## Problem

`MainWindow` sets `Qt::WA_TranslucentBackground` on itself (per-pixel
background alpha drives the user-visible terminal-area opacity
feature). That attribute disables `autoFillBackground` for the whole
widget tree, so any child widget that relies on palette-based or
QSS-based background fill renders over a cleared-to-transparent
surface unless it actively paints something opaque itself.

The menubar collected three distinct failure modes over the 0.6.42 →
0.7.26 window. Each iteration plugged one of them; together they form
the current invariant set.

- **0.6.42** — File/Edit/View hover flash.
  Tried to fix with `QApplication::activePopupWidget()` +
  widget-at-cursor guards in the focus-redirect lambda. Reduced, did
  not eliminate.
- **0.6.43** — explicit `QMenuBar::item { background-color:
  transparent; … }` base rule, so Qt wouldn't fall back to native
  rendering for the non-selected state. Reduced further, did not
  eliminate.
- **0.7.4** — addressed two remaining leaks:
  (a) the menubar widget itself was not opaque w.r.t. its translucent
  parent (added `autoFillBackground` + `WA_StyledBackground` +
  `WA_OpaquePaintEvent`), and
  (b) `::item:hover` was unstyled, so the one-frame gap between the
  style's `:hover` state and `:selected` taking over rendered
  unstyled.
- **0.7.25** — added a belt-and-suspenders `setPalette(...)` +
  `setStyleSheet(...)` block in `applyTheme`. Reported as still
  transparent on KWin + Breeze.
- **0.7.26 (current fix)** — the root cause: under
  `WA_TranslucentBackground` parent + `WA_OpaquePaintEvent` on the
  menubar, **none** of the conventional opaque-paint paths actually
  fires on every WM/style stack:
  * `autoFillBackground` is suppressed by `WA_OpaquePaintEvent` (the
    contract is "the widget paints all pixels").
  * QSS `QMenuBar { background-color: … }` is supposed to draw via
    `QStyleSheetStyle::drawControl(CE_MenuBarEmptyArea)`, but on
    KWin + Breeze + Qt 6 this draw is skipped when
    `WA_OpaquePaintEvent` is set — the QSS engine assumes the widget
    has covered those pixels itself.
  * `QPalette::Window` only feeds `autoFillBackground`, so it
    inherits the same suppression.
  Result: every safeguard from 0.7.25 was in place and the user
  still saw the desktop wallpaper through the menubar strip
  (2026-04-25 report: "I can clearly see my desktop background
  behind it"). The fix is `OpaqueMenuBar` — a `QMenuBar` subclass
  whose `paintEvent` unconditionally `fillRect`s the widget rect
  with the theme's secondary bg color before delegating to
  `QMenuBar::paintEvent`. That is the only path that actually keeps
  the `WA_OpaquePaintEvent` contract honest under translucent
  parents.

## Invariants

The test is source-grep because reproducing the flash and the
transparent-paint reliably needs a full WM + compositor +
translucency stack, which CI doesn't have.

- **INV-1:** `src/mainwindow.cpp` calls
  `m_menuBar->setAutoFillBackground(true)` at construction.
  Kept as paranoia: if a future Qt version stops honoring the
  `WA_OpaquePaintEvent` auto-fill suppression, this gives a second
  opaque layer for free; if it keeps honoring it (today's behavior),
  the call is a no-op.

- **INV-2:** `src/mainwindow.cpp` sets `Qt::WA_StyledBackground` on
  `m_menuBar`. Required so QSS sub-rules (`::item`, `::item:hover`,
  `::item:selected`, `::item:pressed`) are polished on this widget.

- **INV-3:** The QSS block applied to MainWindow defines a
  `QMenuBar::item:hover` selector with a non-transparent
  `background-color` (mirrors the `:selected` highlight). Closes the
  one-frame `:hover`-before-`:selected` gap Breeze / Fusion expose.

- **INV-3b:** `Qt::WA_OpaquePaintEvent` is set on the QMenuBar widget
  at construction. It's a hint to Qt's region tracking that the
  widget covers all its pixels — keeps the compositor from
  invalidating the translucent parent's region under the menubar
  when the menubar repaints. Without this, each mouse-move over the
  menubar item owning an open dropdown causes the compositor to
  damage the popup above the menubar on KWin. This contract is now
  *kept* by `OpaqueMenuBar::paintEvent` (INV-8), not just *promised*
  by the attribute.

- **INV-4:** The `QMenuBar::item` base rule (0.6.43) is still present
  — removing it re-opens the native-rendering-underneath flash.

- **INV-5:** `m_menuBar->setNativeMenuBar(false)` is called so the
  menubar never gets exported to a global-menu D-Bus channel (which
  would hide it entirely in our frameless window).

- **INV-6 (neg):** `QMenu::item` must NOT carry an explicit
  `background-color: transparent` rule. The dropdown QMenu is a
  separate top-level popup with its own non-translucent window and
  does not need the same fix the menubar does — adding the
  transparent-base rule there caused visible redraws of items as
  the current-item selection shifted between rows on mouse move
  (tried mid-0.7.4 session, reverted same day). Keep it pinned so a
  future refactor doesn't re-introduce the regression out of
  "symmetry with QMenuBar".

- **INV-7 (0.7.25):** `MainWindow::applyTheme` sets the menubar's
  `QPalette::Window` to the theme's secondary bg AND applies a
  widget-local `setStyleSheet(QMenuBar ...)` block. Belt-and-
  suspenders for child widgets that the menubar polishes
  (QToolButton, dropdown arrows on style stacks that use them) and
  to scope the `::item` rules on the menubar itself rather than
  relying on the top-level cascade reaching it. NOT load-bearing for
  the strip's opacity any more — that's INV-8 — but kept because
  removing it produces theme-drift on inherited child widgets.

- **INV-8 (0.7.26):** `m_menuBar` is an `OpaqueMenuBar` (not a bare
  `QMenuBar`), and `applyTheme` calls
  `m_menuBar->setBackgroundFill(theme.bgSecondary)`. The subclass's
  `paintEvent` runs `QPainter::fillRect(rect(), m_bg)` with
  `CompositionMode_Source` *before* delegating to
  `QMenuBar::paintEvent` — this is the ONLY path that produces an
  opaque background on KWin + Breeze + Qt 6 under
  `WA_TranslucentBackground` + `WA_OpaquePaintEvent`. User report
  2026-04-25: with INV-1 / INV-2 / INV-3b / INV-7 all already in
  place, the desktop wallpaper still showed through the menubar
  strip. If a future refactor is tempted to "simplify" by reverting
  to a bare `QMenuBar`, re-read this entry first.

## Test scope

The test in this directory is source-grep:

- It does not instantiate Qt — running a real translucent X11/Wayland
  surface in CI is fragile and wouldn't exercise the same compositor
  paths the user hits.
- It pins the structure of the fix at the source level so refactors
  that "tidy up" any of these calls fail loudly with a pointer to
  this spec.

Manual verification before shipping a release that touches these
sites:

1. Pick any non-Plain theme with a dark `bgSecondary`.
2. Set a *bright* desktop wallpaper (so any transparency leak is
   obvious — solid black wallpaper hides this bug entirely).
3. Set `opacity` to ~0.85 in `~/.config/ants-terminal/config.json`
   (forces the WA_TranslucentBackground code path).
4. Launch ants-terminal under KWin (`echo $XDG_CURRENT_DESKTOP`
   should report `KDE`).
5. Confirm the menubar strip is solid `bgSecondary`, not the
   wallpaper showing through. Open and close a dropdown a few times
   while moving the mouse — no flicker on the menubar or above the
   open dropdown.
