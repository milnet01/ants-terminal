# Feature spec: View → Opacity reaches open terminals (ANTS-5034)

The View → Opacity action saved the new level and called
`applyTheme(m_currentTheme)`. `applyTheme` returns early when the theme
name is unchanged, and the config watcher skips a reload of its own write,
so only tabs opened afterwards read the new level.

## Invariants

- **INV-1 — the action updates every live terminal.** The action's handler
  calls `setWindowOpacityLevel` with the chosen level on each terminal in
  `liveTerminals()`.

## Test scope

Source scrape of the action's handler in `MainWindow::setupViewMenu`,
comments stripped. A `MainWindow` cannot be built headless in this bundle.
