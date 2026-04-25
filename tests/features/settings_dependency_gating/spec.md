# Feature: Settings dialog dependency-UI gating

## Problem

Three master checkboxes in the Settings dialog gate sibling
controls whose values are irrelevant when the master is off:

- **AI tab** — `m_aiEnabled` (off by default) gates `m_aiEndpoint`,
  `m_aiApiKey`, `m_aiModel`, `m_aiContextLines`. Pre-fix, the user
  could type an endpoint or paste an API key into a feature-
  disabled dialog with no visible feedback that the value would
  be ignored.
- **Appearance tab** — `m_autoColorScheme` (off by default) gates
  `m_darkThemeCombo` + `m_lightThemeCombo`. Pre-fix, those two
  combos were always live; selecting "Solarized" as the
  light-mode theme appeared to do nothing because auto-switch was
  off — a silent ignored interaction.
- **Terminal tab** — `m_quakeMode` (off by default) gates
  `m_quakeHotkey` and the portal-status label. Pre-fix, typing a
  hotkey into the field had no effect when Quake was disabled.

The fix wires `QCheckBox::toggled → setEnabled(true/false)` on
the dependent siblings, plus a one-shot sync at construction so
the initial state matches the just-loaded config (without
relying on `setChecked()` always emitting `toggled` — it only
emits when the state actually changes).

## Contract

### Invariant 1 — AI tab fields disabled when m_aiEnabled is unchecked

After Settings opens with `ai_enabled=false`, `m_aiEndpoint`,
`m_aiApiKey`, `m_aiModel`, `m_aiContextLines` all return
`isEnabled() == false`. Toggling `m_aiEnabled` to checked
re-enables all four. Toggling back disables them again.

### Invariant 2 — Auto-color-scheme combos gated

After Settings opens with `auto_color_scheme=false`,
`m_darkThemeCombo` and `m_lightThemeCombo` are disabled. Toggling
`m_autoColorScheme` to checked enables both.

### Invariant 3 — Quake hotkey gated

After Settings opens with `quake_mode=false`, `m_quakeHotkey` is
disabled. Toggling `m_quakeMode` to checked enables it.

### Invariant 4 — Initial state matches loaded config

If the user saved `ai_enabled=true` last session, the AI fields
must be ENABLED on next dialog open (not disabled-then-enabled-by-
late-toggle). The sync lambda's one-shot call at construction is
what makes this work; without it, the dialog would briefly show
the AI tab fields disabled until the user toggled the master
checkbox or applied something.

## How this test anchors to reality

Runtime test:

1. Construct a Config with explicit `ai_enabled=false`,
   `auto_color_scheme=false`, `quake_mode=false`.
2. Construct a SettingsDialog from it.
3. Assert each dependent field returns `isEnabled() == false`.
4. Programmatically `setChecked(true)` on each master checkbox.
5. Assert dependents now return `isEnabled() == true`.
6. Repeat with the opposite initial state for I4: construct a
   Config with `ai_enabled=true`, etc., open the dialog, and
   assert dependents are enabled WITHOUT having to manually
   toggle anything.

Source-grep portion: each setup* method must contain `connect(
master, &QCheckBox::toggled, ...)` and a one-shot sync call.

## Regression history

- **Latent since 0.6.x** when each tab landed.
- **Flagged:** ROADMAP audit list — "settings dialog: dependency-
  UI enable gating" at lines `268-286`, `204`, `234-238`.
- **Fixed:** 0.7.32.
