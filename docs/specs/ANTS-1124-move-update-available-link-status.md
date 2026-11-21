# ANTS-1124 — Move "update available" link from status bar to menu bar

**Status:** shipped 0.7.62 (2026-05-01).
**Kind:** ux.

- **Source:** user-2026-04-30
- **Lanes:** MainWindow

## Problem

`m_updateAvailableLabel` (a `QLabel` in `statusBar()->addPermanentWidget(...)`)
competes with the Roadmap / Claude / git-status widgets for the right
edge of the status bar. As a one-shot call-to-action ("you have a new
version") it deserves a louder, less crowded surface.

## Fix

Replace the status-bar `QLabel` with a top-level `QAction` on the menu
bar, added immediately after the `&Help` menu so it appears to its
right under LTR (and to its left under RTL — Qt's menu-bar layout
auto-flips, so "right of Help" is the visual intent under LTR).

- New member: `QAction *m_updateAvailableAction` (replaces
  `QLabel *m_updateAvailableLabel`).
- Capture the release URL — via a member, `QAction::setData`,
  or lambda capture — caller's choice. The `triggered` slot
  replays it through `handleUpdateClicked(url)`.
- The version-check probe (`MainWindow::checkForUpdates`) toggles
  `m_updateAvailableAction->setVisible(true|false)` instead of
  `label->show() / hide()`. No probe-logic changes.
- `triggered` connects to a tiny lambda that calls the existing
  `handleUpdateClicked(url)`. Same Update / Skip / Postpone
  dialog, same in-place AppImageUpdate flow.

## Visual prominence

The action's text (e.g. "↗ Update v0.7.62 available") is naturally
prominent against the normal menu titles because it appears only
when there's something to act on. We don't add bold weight or
color overrides — the menu-bar is a high-attention surface and
the conditional appearance is the differentiator.

## Out of scope

- The version-check probe itself (`checkForUpdates`).
- The in-place update flow (`handleUpdateClicked`).
- The "Check for Updates" Help-menu action — stays where it is;
  user-initiated re-probe, different surface.

## Acceptance (INVs)

Source-grep INVs only — the change is a wiring swap, no new
behaviour to drive. Tests live in
`tests/features/update_available_menubar/`.

- **INV-1** `m_updateAvailableLabel` member is removed from
  `mainwindow.h` (no `QLabel *m_updateAvailableLabel`).
- **INV-2** `m_updateAvailableAction` member exists with type
  `QAction *` in `mainwindow.h`.
- **INV-3** No `statusBar()->addPermanentWidget(m_updateAvailableLabel)`
  or any other status-bar wiring of the label survives in
  `mainwindow.cpp`.
- **INV-4** `mainwindow.cpp` adds the action to the menu bar via
  `m_menuBar->addAction(m_updateAvailableAction)` (top-level
  action, sits after the Help menu).
- **INV-5** `m_updateAvailableAction->setVisible(true)` is called
  in the network-reply path that today calls `label->show()`.
- **INV-6** `m_updateAvailableAction->setVisible(false)` (or
  `->setVisible(false)` via the same path) is called when the
  probe finds the running version is current.
- **INV-7** Click routing: `connect(m_updateAvailableAction,
  &QAction::triggered, ...)` exists and forwards to
  `handleUpdateClicked(...)`.

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that lands ANTS-1124.
git checkout <impl-sha>~1 -- src/mainwindow.cpp src/mainwindow.h
cmake --build build --target test_update_available_menubar
ctest --test-dir build -R update_available_menubar
# Expect INV-1 / INV-2 / INV-3 / INV-4 to fail — pre-fix code
# still has the QLabel in the status bar.
```

## Cold-eyes review fold-in (2026-05-01)

Rows live in [`../reviews/ANTS-1124-move-update-available-link-status-loop-log.md`](../reviews/ANTS-1124-move-update-available-link-status-loop-log.md).
