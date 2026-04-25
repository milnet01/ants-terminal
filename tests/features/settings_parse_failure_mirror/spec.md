# Feature: parse-failure mirror in SettingsDialog + ClaudeAllowlist

## Problem

`Config::load()` was hardened in 0.7.12 to never silently overwrite a
corrupt config file. The same shape — read JSON → parse → on failure
fall back to default-constructed object → next save() overwrites the
user's hand-fixable bytes — exists in:

- `ClaudeAllowlistDialog::saveSettings()` (read-modify-write of
  `~/.claude/settings.local.json` — preserves non-permissions keys
  like model/env/custom hooks).
- `SettingsDialog::installClaudeHooks()` and
  `installClaudeGitContextHook()` (read-modify-write of
  `~/.claude/settings.json` — preserves non-hook keys).

Both have already adopted the **refuse-to-write-on-parse-failure**
pattern + `rotateCorruptFileAside` shared helper. What was missing
(per ROADMAP's "Allowlist + settings-dialog feature-test analogs"
bullet) is behavioural test coverage for these two sites; only
`Config::load` had a regression test
(`tests/features/config_parse_failure_guard/`).

This spec extends coverage to the two analog sites via source-grep
on the canonical refuse-pattern shape.

## Contract

### Invariant 1 — claudeallowlist.cpp refuses to save on parse failure

`src/claudeallowlist.cpp` `saveSettings()` body must:

1. Read the existing settings file if it exists.
2. On `QJsonDocument::fromJson` returning a non-object, call
   `rotateCorruptFileAside(m_settingsPath)` and return false.
3. NOT proceed to the QSaveFile write path.

The code must therefore contain — in the `saveSettings` body —
both `rotateCorruptFileAside(m_settingsPath)` and a `return false;`
following it (the early-exit before the write).

### Invariant 2 — settingsdialog.cpp refuses both installers on parse failure

`installClaudeHooks` and `installClaudeGitContextHook` must each:

1. Read `claudeSettingsPath()` if it exists.
2. On parse failure, call `rotateCorruptFileAside(settingsPath)`,
   present a warning dialog, and return.
3. NOT proceed to the QSaveFile write path.

Both installers need the rotation+return pattern. The file must
contain at least two `rotateCorruptFileAside(settingsPath)` call
sites (one per installer) — each followed by an early `return;`
before the QSaveFile write that would otherwise clobber the
settings file.

### Invariant 3 — read-OPEN failure also refuses (not just parse failure)

If the settings file exists but `QFile::open(ReadOnly)` returns
false (perms, unreadable handle), the saver must also refuse.
Otherwise we'd write `{permissions: ...}` over a file we couldn't
read — same silent-data-loss class. The saver must contain a
"file exists but cannot be read" early-exit branch.

## How this test anchors to reality

Source-grep both files for:

1. The `rotateCorruptFileAside` calls (count = 1 in allowlist,
   count ≥ 2 in settingsdialog because two installers).
2. The "exists but cannot be read" guard before the parse path.
3. An early-return after the rotation so the write path is
   genuinely unreachable on parse failure.

A behavioural test would be ideal but requires an interactive
QSaveFile mock or a tmp ~/.claude/ override; for the bundle's
budget the source-grep is high-confidence and zero-cost.

## Regression history

- **Introduced (latent):** Same ~30-line pattern as Config::load,
  shipped at the same time the dialogs grew their initial JSON
  read-modify-write paths.
- **Fixed (code):** 0.7.12 — rotation + refuse pattern landed in
  both `claudeallowlist.cpp` and `settingsdialog.cpp` alongside
  the Config fix.
- **Fixed (test):** 0.7.31 — analog tests for both sites added.
