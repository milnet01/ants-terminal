# audit_state_file_caps — the audit dialog skips oversized project files

**Bundle:** `test_dialogs` · **Suite:** `AuditStateFileCaps` · **Label:** `features;fast`

## Problem

`AuditDialog` reads four project files whole on the GUI thread, with no size
limit: `audit_rules.json`, `.audit_suppress`, `.audit_cache/baseline.json` and
`.audit_cache/trend.json`. A cloned repository controls all four, and
`audit_rules.json` is read and hashed before its trust check. A planted
multi-gigabyte file stalls or exhausts the process when the dialog opens.

## Surface

Every read of those files goes through `readAuditStateFile`, which refuses a
file larger than `kMaxAuditStateFileBytes`. A refused file is treated as
absent, with a warning on stderr.

## Invariants

- **INV-1 — an oversized rule pack is skipped.** A trusted `audit_rules.json`
  holding one valid rule, padded past the cap with trailing whitespace, adds
  no check to the catalogue.
- **INV-2 — a normal rule pack still loads.** The same rule without the
  padding adds its check.
- **INV-3 — every reader uses the capped read.** In the AuditDialog sources,
  `loadUserRules`, `loadSuppressions`, `saveSuppression`, `loadBaseline`,
  `loadLastSnapshot` and `appendSnapshot` each call `readAuditStateFile`, and
  none calls `readAll()`. When `saveSuppression` cannot read an oversized
  `.audit_suppress`, it skips the legacy-format conversion and appends.

## Test surface

INV-1 and INV-2 construct an `AuditDialog` on a temporary project with
`ANTS_AUDIT_TRUST_UNSAFE=1` and read `checksForTest()`. INV-3 reads the source
text, because the other three files have no test accessor.

## Out of scope

- The value of the cap. INV-1 pads well past it.
- Files other tools write under `.audit_cache/`.

## Regression history

- **ANTS-5083:** the four files were read with no size cap. Fixed by
  `readAuditStateFile`.
