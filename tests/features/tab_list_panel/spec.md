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
  `tab_list_panel_width` (default 280) round-trip through `Config`, as
  does `tab_list_panel_sort` (INV-10).
- **INV-7 — live reload.** Both `onConfigFileChanged` and the Settings
  dialog's `settingsChanged` handler call `applyTabListPanelConfig`, so a
  change applies with no relaunch.
- **INV-8 — state as row colour (ANTS-5650).** A tab row's background is
  `ClaudeTabIndicator::color` of its glyph, or the panel background for
  `None`. At rest a state row's background is solid under the name, then
  fades left to right into the panel background (ANTS-5658). Its tab
  colour is a stripe at the left edge, `max(5, height / 4) + 3` pixels
  wide. The name is drawn
  in black or white, whichever has the higher contrast with the
  state colour, and that contrast is at least 4.5:1. Under the mouse the
  whole row fills with one changed colour, with no fade, and the 4.5:1
  floor still holds, selected or not.
  The current tab's row is bold, with no outline and no highlight over
  its colour: after the name it fades into the inverse of the panel
  background, whatever its state (ANTS-5660). The panel paints tab rows itself, so the app style sheet's
  item rules do not reach them.
- **INV-9 — rename from the panel.** A right-click on a tab row emits
  `tabContextMenuRequested` with its page; a header emits nothing. The
  MainWindow answers with the tab bar's menu, `showTabColorMenu(i, true)`,
  whose Rename calls `startRename`, because the tab bar may be hidden.
  `startRename` opens an editor on the row holding its whole title, and
  `setEntries` does not rebuild the rows while it is open. Closing it with
  a changed name emits `tabRenamed` once; Escape emits nothing and keeps
  the old title. The MainWindow pins the name with `setTabTitleForRemote`.
- **INV-10 — sort order (ANTS-5656).** `setSortMode` picks one of four
  orders, and `tab_list_panel_sort` stores it: `colour` (the default, and
  any unknown value) is INV-2; `colour_az` is INV-2 with each group's rows
  A–Z; `az` lists every row A–Z with no headers; `tab_order` lists every
  row in tab order with no headers. A–Z ignores case, puts numbers in
  number order ("Tab 2" before "Tab 10"), and keeps tab order between
  equal names. Sorting changes the panel only, never the tab order. The
  View menu's "Tab List Sort" sets it, and `applyTabListPanelConfig`
  passes it to the panel, so a change applies with no relaunch.
- **INV-11 — the tab bar's visibility (ANTS-5659).** The tab bar shows
  only with two or more tabs, and never while the panel is shown with
  `tab_list_panel_hides_tab_bar` set. `MainWindow::updateTabBarVisibility`
  is the only code that shows or hides it, and every change to the tab
  count or the panel settings calls it.
- **INV-12 — close from the panel (ANTS-5709).** Every tab row draws a
  close mark at its right edge, in `TabListPanel::closeButtonRect` of the
  row's rectangle. A left click there emits `tabCloseRequested` with the
  row's page and does not emit `tabActivated`. A click elsewhere on the row
  switches as INV-3 says and closes nothing. Headers have no close mark.
  The MainWindow answers `tabCloseRequested` with `closeTab`, the tab bar's
  x path, so confirm-on-close still applies.
- **INV-13 — the tab menu (ANTS-5709).** `showTabColorMenu`, shared by the
  tab bar and the panel, has a "Close Tab" item that calls `closeTab`. Its
  colour choices sit in a "Colour" sub-menu, not on the top level.

## Test

`test_tab_list_panel.cpp`, suite `TabListPanel`, in the `test_chrome`
bundle. INV-7, INV-11 and the second half of INV-1 read the MainWindow
sources.
