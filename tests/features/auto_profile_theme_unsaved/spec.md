# Feature: an auto-profile theme is not saved (ANTS-6489)

An auto-profile rule (`auto_profile_rules` in `config.json`) applies its
profile's theme to the window while the rule's tab matches. The theme
the user chose stays the saved one, as the profile's font size, opacity
and badge already do: none of them is written to `config.json`.

## What this test asserts

Source-scrape, since no test constructs a `MainWindow`.

- **INV-1** — `MainWindow::checkAutoProfileRules` calls `applyTheme` with
  `ThemeSave::Unsaved` and never calls `m_config.setTheme`.
- **INV-2** — `MainWindow::applyTheme` saves only for `ThemeSave::Saved`,
  on its early return for the theme already showing too, so choosing that
  theme from the menu still saves it. The popup deferral carries the flag.
- **INV-3** — `MainWindow::onConfigFileChanged` applies the reloaded theme
  only when it differs from the theme saved before the reload. A hand
  edit of another setting keeps the profile's theme on screen.
