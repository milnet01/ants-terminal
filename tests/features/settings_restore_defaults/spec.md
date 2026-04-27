# Feature: Settings dialog has Restore Defaults per tab

## Problem

The Keybindings tab has a "Reset to Defaults" button (shipped
0.6.x). The General / Appearance / Terminal / AI tabs did not —
a user who tweaked the dialog and wanted to start over had to
either remember every default or delete `~/.config/ants-terminal/
config.json` and lose unrelated settings (highlight rules,
profiles, etc.) along the way.

The fix gives each substantive tab its own "Restore Defaults"
button that resets ONLY that tab's controls to the schema
defaults defined in `Config::xxx()` getters' default arguments.
Resets land in the dialog widgets only — applySettings still
needs to run for the changes to reach m_config, so Cancel rolls
the reset back the same way it does any other in-dialog edit.

## Contract

### Invariant 1 — General / Appearance / Terminal / AI tabs each have a button

Each setup* method for those four tabs must construct a
QPushButton with the title pattern "Restore Defaults (<TabName>
tab)" and add it to the tab's layout. The button has a stable
objectName (`restoreDefaultsGeneral`, `restoreDefaultsAppearance`,
`restoreDefaultsTerminal`, `restoreDefaultsAi`) so the runtime
test can locate it without depending on the layout structure.

### Invariant 2 — Each button resets the tab's full control set

The slot connected to each button must reset every primary
control on that tab. The reset values match the schema defaults:

- General: shellCombo idx 0, sessionPersistence true, sessionLogging
  false, autoCopy true, confirmMultilinePaste true, editorCmd
  empty, imagePasteDir empty, notificationTimeout 5,
  claudeTabStatusIndicator true, tabTitleFormat "title".
- Appearance: fontSize 11, theme "Dark", opacity 100,
  backgroundBlur off, terminalPadding 4,
  badgeText empty, autoColorScheme off, darkTheme "Dark",
  lightTheme "Light".
- Terminal: scrollbackLines 50000, showCommandMarks true,
  quakeMode off, quakeHotkey "F12".
- AI: aiEnabled false, aiEndpoint empty, aiApiKey empty, aiModel
  "llama3", aiContextLines 50.

### Invariant 3 — Reset doesn't bypass the OK / Cancel pattern

The reset slot mutates widget state only. It does NOT call
`m_config->setXxx()` directly. applySettings remains the only
path that reaches m_config; Cancel rolls the reset back along
with any other in-dialog edits.

## How this test anchors to reality

Source-grep:

1. Each setup* method declares a `QPushButton` with the matching
   `setObjectName(...)` call.
2. Each reset slot's lambda body sets the documented widget
   values for that tab.
3. None of the reset slot bodies call `m_config->`. The whole
   block is widget-only.

## Regression history

- **Latent since the original Settings dialog landed in 0.5.x.**
- **Flagged:** ROADMAP "settings dialog: Restore Defaults
  per-tab".
- **Fixed:** 0.7.32.
