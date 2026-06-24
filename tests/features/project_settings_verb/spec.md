# Feature: layout auto-detect + `project_settings` verb

Test contract for ANTS-2161 (`docs/specs/ANTS-2161.md`). Locks the pure
`ProjectSettings::detect` detector and `ProjectSettings::applyWrite`
writer/validator, plus the verb-layer + registration wiring via
source-grep.

`detect` / `applyWrite` are pure Qt6::Core helpers; the test drives them
directly against `QTemporaryDir` fixtures (a canonicalised root per case).
The verb glue (`cmdProjectSettings`) and its dispatch registration are not
unit-testable without `RemoteControl`/`MainWindow`, so the verb-layer
invariants (INV-5/6/10/13) are asserted by source-grep — the same shape
the ANTS-2160 suite used for its consumer-wiring check.

## Invariants under test (mirrors ANTS-2161 §3)

- **INV-1** — standard `src/` layout, no settings file → `detect` returns
  no suggestion (`present:false`, `sourceRoots` nullopt, empty reason).
- **INV-2** — an existing `.ants/project.json` → `present:true`, no
  suggestion (the detector never second-guesses a configured project).
- **INV-3** — code under `engine/` (no `src/`) → suggests
  `source_roots:["engine"]`, `defaultSourceCount 0`, `totalSourceCount 2`.
- **INV-4** — `applyWrite` + a real write round-trips through `load`; the
  file is world-readable (0644).
- **INV-7** — `set` merge preserves untouched keys, including unrecognised
  ones (forward-compat).
- **INV-8** — a `null` value removes a key; a `null`-only call on an
  already-absent key is a valid no-op (object returned, not `bad_args`).
- **INV-9** — write-time validation: absent / wrong-type / root-escape →
  `bad_path`; wrong shape (non-array) → `bad_args`; never writes on fail.
- **INV-11** — noise dirs (`node_modules/` …) skipped: neither suggested
  nor counted in the total.
- **INV-12** — source at the repo root (no subdir) → no suggestion.
- **INV-14** — dominance gate: a miss with no subdir set reaching the bar
  (half the source at the repo root) → no suggestion.
- **INV-5/6/10/13 (wiring)** — `project_settings` registered
  `CallerCwdContract::Required` in `claudeintegration.cpp`'s
  `callerCwdContractFor()` + `registerToolProvider`; the handler writes
  `.ants/project.json`, refuses `settings_exists` (init no-clobber),
  returns `written:false` (init no-op), refuses `unrecognised_format`
  (malformed existing), and uses `bad_args` for set's no-key guard.

## Pre-fix check

Against pre-implementation code `ProjectSettings::detect`/`applyWrite` do
not exist (compile error) and the verb/registration grep strings are
absent, so every assertion fails. Verified before wiring the
implementation.

Label: `features;fast`.
