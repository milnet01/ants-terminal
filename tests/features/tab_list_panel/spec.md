# ANTS-5645 — the tab list panel

A side panel lists every open tab by its full name, so a window with more
tabs than the tab bar can show needs no sideways scrolling. The roadmap
item records the user's choice of a side panel over wrapping rows.

## Invariants

- **INV-1 — one row per tab, full name.** `TabListPanel::setEntries` shows
  one row per entry, in the order given, with the entry's whole title. The
  MainWindow keeps that whole title through `setTabLabel`, which is the
  only place a tab label is cut to fit the tab bar.
- **INV-2 — grouped by colour.** A tab's group is its tab colour (the
  `tab_groups` config key holds colours, not names). When any entry has a
  colour, rows sit under one header per colour, in the order of each
  colour's first tab. A header names the colour from
  `ColoredTabBar::palette()`, or reads "Custom colour". Uncoloured tabs go
  last, under "No colour". When no entry has a colour there are no headers.
- **INV-3 — click to switch.** Clicking or activating a tab row emits
  `tabActivated` with that row's page. Headers cannot be selected. The
  current tab's row is the selected row.
- **INV-4 — Claude state.** A row whose Claude glyph is not `None` names
  the state in words in its tooltip and accessible text, since the colour
  alone does not say it. A row with glyph `None` names no state.
- **INV-5 — readable size.** `setFontPointSize` sets the rows' font size.
  The MainWindow passes the terminal font size, so the panel follows it.
- **INV-6 — config keys.** `tab_list_panel` (default false),
  `tab_list_panel_hides_tab_bar` (default false) and
  `tab_list_panel_width` (default 280) round-trip through `Config`.
- **INV-7 — live reload.** Both `onConfigFileChanged` and the Settings
  dialog's `settingsChanged` handler call `applyTabListPanelConfig`, so a
  change applies with no relaunch.
- **INV-8 — state as row colour (ANTS-5650).** A tab row's background is
  `ClaudeTabIndicator::color` of its glyph, or the panel background for
  `None`. Its tab colour is a stripe at the left edge. The name is drawn
  in black or white, whichever has the higher contrast with the
  background, and that contrast is at least 4.5:1. Under the mouse the
  background changes and the 4.5:1 floor still holds, selected or not.
  The current tab's row is bold and outlined, with no highlight over its
  colour. The panel paints tab rows itself, so the app style sheet's
  item rules do not reach them.

## Test

`test_tab_list_panel.cpp`, suite `TabListPanel`, in the `test_chrome`
bundle. INV-7 and the second half of INV-1 read the MainWindow sources.
