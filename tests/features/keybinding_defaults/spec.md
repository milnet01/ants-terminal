# Feature: one table of default keyboard shortcuts (ANTS-6146)

## Problem

The default shortcut of each configurable action was written in three
places: each menu action's `m_config.keybinding("<action>", "<key>")`
call, the Settings Keybindings table's load, and its "Reset to Defaults"
button. They drifted. The menus bound the bookmark jumps to
Ctrl+Alt+Down/Up (ANTS-1165) while Settings listed, and Reset restored,
Ctrl+Shift+Down/Up, a chord the terminal takes for prompt navigation.
Eight menu shortcuts were missing from the Settings table.

## Contract

- **INV-1** — `Config::defaultKeybindings()` names each action once and
  gives no two actions the same default chord.
- **INV-2** — `next_bookmark` and `prev_bookmark` default to
  Ctrl+Alt+Down and Ctrl+Alt+Up.
- **INV-3** — every `keybinding("<action>"` call in the MainWindow
  sources names an action in the table and passes no default of its own.
- **INV-4** — `src/settingsdialog.cpp` reads `Config::defaultKeybindings()`
  and holds no shortcut table of its own.

## Test

`test_keybinding_defaults.cpp`, suite `KeybindingDefaults`, in the
`test_chrome` bundle. INV-1 and INV-2 read the table at run time; INV-3
and INV-4 read the source.
