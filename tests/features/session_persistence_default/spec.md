# Feature: `session_persistence` defaults ON

## Contract

`Config::sessionPersistence()` controls whether ants-terminal saves
scrollback + tab state on exit and restores them on next launch via
`SessionManager`. Modern terminal emulators (iTerm2, WezTerm, Kitty,
Konsole) restore tabs between sessions by default; ants-terminal does
the same. The accessor MUST honour the following three invariants:

1. **INV-1 — Fresh users get persistence.** When the backing JSON
   object has *no* `session_persistence` key (a user who has never
   toggled the Settings checkbox, e.g. a first-run install),
   `Config::sessionPersistence()` returns `true`. This is the fix for
   the user-reported regression "The terminal is no longer remembering
   tabs between sessions" — the prior default was `false`, so anyone
   who never opened Settings had their tabs quietly thrown away on
   every exit.

2. **INV-2 — Explicit opt-out is respected.** When the backing JSON
   contains `"session_persistence": false`, `sessionPersistence()`
   returns `false`. Users who have deliberately turned the feature
   off must continue to get what they asked for; the default only
   applies to the *absence* of the key.

3. **INV-3 — Explicit opt-in is respected.** When the backing JSON
   contains `"session_persistence": true`, `sessionPersistence()`
   returns `true`. A trivial symmetry check that nails down the
   accessor's response to explicit settings and prevents any
   "default inverted" class of bug.

## Rationale

Pre-0.6.34, `Config::sessionPersistence()` used `.toBool(false)` as
the default fallback when the key was missing. Users reported tabs
vanishing between runs; root cause was that fresh configs never had
the key, so the `false` default silenced `SessionManager` entirely.
The fix flips the fallback to `.toBool(true)`; this spec locks that
default in so a future refactor or accidental `.toBool(false)` revert
gets caught by the CI feature lane before landing.

The three invariants together form an exhaustive truth table for the
accessor: {key missing, key=false, key=true} → {true, false, true}.

## Scope

### In scope
- The `Config::sessionPersistence()` getter with all three possible
  JSON backing states, driven directly by writing
  `config.json` under an isolated `XDG_CONFIG_HOME` before
  constructing the `Config`.

### Out of scope
- `SessionManager::saveAll` / `restoreAll` behaviour — covered by
  other tests; here we only prove the *gate* defaults correctly.
- The Settings dialog's checkbox wiring. The accessor is the source
  of truth; UI callers read it.
- File-format migration of pre-0.6.34 configs that explicitly wrote
  `"session_persistence": false`. Those users opted out deliberately
  (even if they did so via the old default-propagation); INV-2
  guarantees we respect that.

## Regression history

- **≤ 0.6.33:** `Config::sessionPersistence()` returned
  `m_data.value("session_persistence").toBool(false)`. Fresh users
  never got session restore. User report: "The terminal is no longer
  remembering tabs between sessions."
- **0.6.34:** default flipped to `true` at
  `src/config.cpp:310` (function body now
  `return m_data.value("session_persistence").toBool(true);`). This
  test locks the new default in place.
