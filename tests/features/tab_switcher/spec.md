# ANTS-5646 — go to a tab by typing its name

A keyboard shortcut opens the command palette in a tab mode: one row per
open tab, filtered as you type, and Enter switches to the chosen tab. It
reuses the command palette rather than adding a second popup, as the
roadmap item asked.

## Invariants

- **INV-1 — one row per tab, as written.** `CommandPalette::showTabs`
  lists one row per action given, in order, with the action's text
  exactly. Tab mode does not strip `&`, which the command list removes
  as a menu accelerator.
- **INV-2 — type to filter.** Typed text keeps the rows whose text
  contains it, ignoring case. The first row is selected.
- **INV-3 — Enter switches.** Enter triggers the selected row's action
  and closes the palette. The action still fires when a `closed()` slot
  rebuilds the command list, so MainWindow's cleanup of the tab actions
  must not run before it: it is deferred with `QTimer::singleShot`.
- **INV-4 — readable rows.** Tab mode sets the font size it is given on
  the input and the rows, and shows each action's icon. The icon is
  `TabListPanel::tabIcon`, the side panel's colour stripe and Claude dot.
- **INV-5 — back to commands.** Closing tab mode restores the command
  placeholder and font, and leaves tab mode.
- **INV-6 — wiring.** The View menu has a "Go to Tab" action on the
  `tab_switcher` keybinding. Its default is in `Config::defaultKeybindings()`,
  the table the Settings shortcut list reads (ANTS-6146).

## Test

`test_tab_switcher.cpp`, suite `TabSwitcher`, in the `test_chrome`
bundle. INV-3's deferral and INV-6 read the MainWindow and Config
sources.
