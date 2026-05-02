# UI / chrome state persistence — Phase 1 (ANTS-1150)

Companion test for `docs/specs/ANTS-1150.md`. The full design
rationale, defaults table, edge-case fallback (Custom + empty
status filters → Full), and the cold-eyes-folded race / ordering
analysis live in the spec; this file documents only what
`test_ui_state_persistence.cpp` itself asserts.

The test is a hybrid:

1. **Round-trip lane** — link `Qt6::Core/Gui` + `src/config.cpp`,
   sandbox `XDG_CONFIG_HOME` via `QTemporaryDir`, instantiate
   `Config`, call each new setter, destroy + reconstruct (forces
   `Config::load()` to read back from disk), assert the getter
   returns the persisted value.

2. **Wiring lane (source-grep)** — read
   `src/{settingsdialog,roadmapdialog,auditdialog,mainwindow}.{cpp,h}`,
   assert each dialog calls the new setter on the right signal and
   reads the new getter on ctor.

INV labels are qualified `ANTS-1150-INV-N` to avoid cross-spec
collision.

## Invariants

### Round-trip lane (links Config)

- **ANTS-1150-INV-1** `Config::setSettingsDialogLastTab(int)` /
  `settingsDialogLastTab()` round-trip survives a Config
  destruct + reconstruct cycle. Verifies for value 5 (non-default)
  and value 0 (default-equal).

- **ANTS-1150-INV-2** `setRoadmapActivePreset` /
  `roadmapActivePreset` round-trip for `"current"` and the unknown-
  string fallback (a stored bogus value reads back as `"full"`).

- **ANTS-1150-INV-3** `setRoadmapKindFilters({"fix","audit-fix"})`
  round-trip preserves both values. The on-disk array is sorted
  (write-side `kinds.sort()` per spec) so the byte ordering is
  stable across saves regardless of `QSet` internal iteration
  order.

- **ANTS-1150-INV-4** `setRoadmapStatusFilters` round-trip with a
  five-key object (mixed true/false) preserves every key.

- **ANTS-1150-INV-5** `setAuditSeverityFilters` round-trip with
  three-on / two-off matches on every key.

- **ANTS-1150-INV-6** `setAuditShowNewOnly(true)` round-trip
  preserves the bool.

- **ANTS-1150-INV-7** `storeIfChanged` short-circuit at the
  in-memory layer: every new setter, called twice with the same
  value, leaves `Config::rawData()` byte-identical between calls.
  Asserts at the in-memory layer (not via filesystem mtime —
  resolution varies across filesystems and CI configs). Defends
  the inotify-fileChanged loop class.

- **ANTS-1150-INV-8** First-launch defaults: a fresh `Config` with
  no `config.json` returns the documented defaults (0, "full",
  empty list, empty object × 2, false) without throwing.

### Wiring lane (source-grep)

- **ANTS-1150-INV-9** `settingsdialog.cpp` calls
  `setSettingsDialogLastTab(` AND installs a `currentChanged`
  connect on `m_tabs`. Restore call is wrapped in
  `QSignalBlocker` (token: `QSignalBlocker block(m_tabs)` or
  equivalent on the same scope as the `setCurrentIndex` call).

- **ANTS-1150-INV-10** `roadmapdialog.cpp` calls
  `setRoadmapActivePreset(` from BOTH `applyPreset` and
  `onCheckboxToggled` (the two write sites for `m_activePreset`).
  Verified by extracting each function body and asserting the
  setter call appears inside.

- **ANTS-1150-INV-11** `roadmapdialog.cpp` calls
  `setRoadmapKindFilters(` AND `setRoadmapStatusFilters(`.

- **ANTS-1150-INV-12** `roadmapdialog.cpp` ctor restores all
  three persisted RoadmapDialog keys (greps for the three
  getters: `roadmapKindFilters()`, `roadmapStatusFilters()`,
  `roadmapActivePreset()`). The restore order asserts that
  `roadmapStatusFilters()` is read inside an
  `if (... == Preset::Custom ...)` guard (the named-preset
  path skips status restore per the cold-eyes CRITICAL #1
  fold-in).

- **ANTS-1150-INV-13** `auditdialog.h` ctor signature includes
  `Config *config` (third param, no `= nullptr` default per
  cold-eyes HIGH #3).

- **ANTS-1150-INV-14** `auditdialog.cpp` calls
  `setAuditSeverityFilters(` AND `setAuditShowNewOnly(`, AND the
  ctor restore reads `auditSeverityFilters()` AND
  `auditShowNewOnly()`.

- **ANTS-1150-INV-15** `mainwindow.cpp` passes `&m_config` to the
  `new AuditDialog(...)` ctor call. Multi-line-aware grep — the
  test reads the file and searches the 4 lines starting at the
  first `new AuditDialog(` for the `&m_config` token.

## How to verify pre-fix code fails

Each round-trip INV (1-6) currently fails at link time because the
new getter / setter symbols don't exist on `Config`. To verify the
test reproduces the gap, comment out the new prototypes in
`src/config.h` and re-run `ctest -R ui_state_persistence` — expect
a link error. Once the cohort lands, INVs 1-15 all pass.

INVs 7-8 are construction-only and pass against pre-fix code as
soon as the new getters exist (their semantics are
default-on-empty + idempotent-on-repeat, which the pattern
guarantees).

INVs 9-15 are source-grep — they fail until the wiring is added.
