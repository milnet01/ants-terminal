# settings_bad_pattern_warning — Settings warns about a pattern it would not save

**Bundle:** `test_dialogs` · **Suite:** `SettingsBadPatternWarning`

ANTS-6979, row 246 of `docs/reviews/close-findings-2026-11-06.md`. A
Highlights or Triggers row whose pattern does not compile, or has the
catastrophic-regex shape, used to vanish on save with no message.

## Invariants

- **INV-1 — each bad pattern is named.**
  `SettingsDialog::unusablePatterns(tab, patterns)` returns one line per
  pattern that would not be saved. Each line names the tab, the pattern and
  why: Qt's `errorString()` for a pattern that does not compile, "it could
  freeze the terminal" for the catastrophic shape. Empty and valid patterns
  give no line.
- **INV-2 — OK and Apply save nothing while one is bad.** Both button
  handlers call `patternsSaveable()` and return before `applySettings()`
  when it is false, so the dialog stays open with every row as typed.
  `patternsSaveable()` shows one warning titled "Settings not saved".

## Test surface

INV-1 calls the static function; no dialog is built. INV-2 reads
`settingsdialog.cpp` with comments stripped.
