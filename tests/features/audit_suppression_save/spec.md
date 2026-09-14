# Feature: audit suppressions take effect only when saved, and read fresh lines

## Invariants

**INV-1 — each run reads source lines fresh.** `AuditDialog::runAudit` clears
`m_fileLineCache` before the run starts, so an inline suppression added since
the last run is seen.

**INV-2 — a suppression that was not saved does not hide the finding.**
`AuditDialog::saveSuppression` checks the rewrite's `write` and `commit()` and
the append's writes and flush. Only when the save succeeded does it add the
key to `m_suppressedKeys`; otherwise it reports the failure and returns.

## Rationale

`inlineSuppressed()` reads source lines through `m_fileLineCache`, which only
`renderResults` cleared, so a run reused the previous run's lines.
`saveSuppression` ignored `QSaveFile::commit()` and the append's write results,
then marked the key suppressed regardless.

## Test surface

`test_audit_suppression_save.cpp` reads `src/auditdialog.cpp` (located from the
test's own path) and checks the two function bodies. The existing
`audit_sarif_suppressions` and `audit_dedup_96bit` checks on the same bodies
still hold.

## Regression history

- **ANTS-5083:** the two defects above. Locked by this spec.
