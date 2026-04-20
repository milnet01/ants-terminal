# Menubar hover stylesheet — no-flicker invariants

## Problem

`MainWindow` sets `Qt::WA_TranslucentBackground` on itself (per-pixel
background alpha is a user-visible feature). That attribute disables
`autoFillBackground` for the whole widget tree, so any child widget
that relies on palette-based background fill renders over transparent
pixels for one frame during repaints.

The menubar hit this twice:

- **0.6.42** tried to fix File/Edit/View hover flash by adding
  `QApplication::activePopupWidget()` + widget-at-cursor guards to the
  focus-redirect lambda. Reduced, did not eliminate.
- **0.6.43** added an explicit `QMenuBar::item { background-color:
  transparent; … }` base rule so Qt wouldn't fall back to native
  rendering for the non-selected state. Reduced further, did not
  eliminate.
- **0.7.4** (this fix) addresses the two remaining leaks:
  (a) the menubar itself wasn't opaque with respect to its
  translucent parent, and
  (b) `::item:hover` was never styled, so the one-frame gap between
  a style's `:hover` state kicking in and `:selected` taking over
  showed unstyled rendering.

## Invariants

The test is source-grep because reproducing the flash reliably needs a
full WM + compositor + translucency stack, which CI doesn't have.

- **INV-1:** `src/mainwindow.cpp` calls
  `m_menuBar->setAutoFillBackground(true)` at construction.
- **INV-2:** `src/mainwindow.cpp` sets `Qt::WA_StyledBackground` on
  `m_menuBar`.
- **INV-3:** The QSS block applied to MainWindow defines a
  `QMenuBar::item:hover` selector with a non-transparent
  `background-color` (mirrors the `:selected` highlight). Closes the
  one-frame `:hover`-before-`:selected` gap Breeze / Fusion expose.
- **INV-3b:** `Qt::WA_OpaquePaintEvent` is set on the QMenuBar widget
  at construction. The menubar's `autoFillBackground` + stylesheet
  fully cover every pixel on every paint, so marking it opaque-on-
  paint stops Qt from invalidating the translucent parent's
  compositor region under it when it repaints. This is the fix for
  the open-dropdown flicker: without it, each mouse-move over the
  menubar item owning the open dropdown causes the compositor to
  damage the popup above the menubar on KWin.
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
