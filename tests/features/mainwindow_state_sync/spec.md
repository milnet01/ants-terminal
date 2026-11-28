# Feature: the window's state follows what changed (ANTS-6636)

A change in one place reaches every part of the window that shows it, and
only that part.

## Invariants

**INV-1 — a background tab stays in the background.** In
`MainWindow::cleanupEmptySplitters`, promoting a tab root's last pane records
`m_tabWidget->currentWidget()` before `removeTab` and restores it afterwards
with `setCurrentWidget`. It never calls `setCurrentIndex(tabIdx)`, which
switched the user to the tab whose shell had exited.

**INV-2 — Reload User Themes refreshes the command palette.** The handler
calls `rebuildCommandPalette()` after `addThemeActions(themesMenu)`, so the
palette loses its proxies for the old theme actions and gains the new ones.

**INV-3 — the auto-profile regex cache is per window.**
`MainWindow::checkAutoProfileRules` compares `m_config`'s rules generation
with the member `m_autoProfileRulesGenSeen`, not a function-local static.
Each window has its own `Config`, so a shared last-seen generation made two
windows clear the shared caches on every tick.

**INV-4 — the system light/dark signal is always connected.** The
constructor connects `QStyleHints::colorSchemeChanged` whatever
`autoColorScheme()` reads; `onSystemColorSchemeChanged` checks the setting.
Turning the setting on later then takes effect without a restart.

## Test surface

`test_mainwindow_state_sync.cpp` reads the MainWindow sources
(`ants_test::slurpMainWindow()`), comments stripped, and checks the function
bodies as text. No `MainWindow` is constructed: each needs a window with
tabs, a palette, two windows, or a system colour-scheme change.

## Regression history

- **ANTS-6636:** the four defects above, found by the 2026-11-06 reviews.
  Locked by this spec.
