# Feature: Settings Profiles tab honors Cancel / OK semantics

## Problem

Pre-fix, the Profiles tab's Save / Delete / Load buttons all
mutated `m_config` immediately:

- Save → `m_config->setProfiles(profiles)` writes a new profile to
  config.json before the user clicks OK / Apply / Cancel.
- Delete → `m_config->setProfiles(profiles)` removes a profile
  immediately.
- Load → `m_config->setActiveProfile(name)` records the active
  profile immediately.

Cancel could not roll back any of those edits — they had already
been persisted via the setter→save() chain. Every other tab
follows the standard "stage in widgets, commit on
applySettings(), discard on reject" pattern; Profiles broke that
contract.

The fix routes all three buttons through a pending-state pair:

```
QJsonObject m_pendingProfiles;
QString     m_pendingActiveProfile;
```

`loadSettings()` initializes them from `m_config`; the buttons
mutate the pending pair only; `applySettings()` is the single
point where they reach `m_config->setProfiles` /
`setActiveProfile`. Cancel skips applySettings, so the dialog
closes leaving `m_config` unchanged.

## Contract

### Invariant 1 — Save mutates pending only, not m_config

Clicking the Save button while typing a profile name must result
in `m_pendingProfiles[name]` being populated, and m_config's
profiles dict must NOT yet contain that name. After Cancel, the
profile must still not be in m_config.

### Invariant 2 — Delete mutates pending only

Clicking Delete on an existing profile must remove it from
`m_pendingProfiles` but leave m_config's copy intact until OK /
Apply.

### Invariant 3 — Load mutates pending active profile only

Clicking Load must set `m_pendingActiveProfile = name` but not
`m_config->setActiveProfile`.

### Invariant 4 — applySettings commits the pending pair

`applySettings()` must call both `setProfiles(m_pendingProfiles)`
and `setActiveProfile(m_pendingActiveProfile)`. Without both
calls, OK / Apply leave the staged edits dangling.

### Invariant 5 — loadSettings re-initializes pending from m_config

Each time the dialog is opened, `m_pendingProfiles` and
`m_pendingActiveProfile` are reset from `m_config->profiles()` /
`activeProfile()`. Stale state from a previous Cancel must not
leak between dialog instances.

## How this test anchors to reality

Source-grep:

1. `m_pendingProfiles` and `m_pendingActiveProfile` declared in
   `src/settingsdialog.h`.
2. The three button slots (Save / Delete / Load) mutate the
   pending pair, not `m_config->setProfiles` directly. Specifically
   the slots must NOT contain `m_config->setProfiles(` —
   that call should appear ONLY inside applySettings.
3. `applySettings` calls `setProfiles(m_pendingProfiles)` and
   `setActiveProfile(m_pendingActiveProfile)`.
4. `loadSettings` assigns to both pending members from m_config.

## Regression history

- **Latent since the Profiles tab landed.**
- **Flagged:** ROADMAP audit, "settings dialog: cancel rollback
  for profiles tab".
- **Fixed:** 0.7.32.
