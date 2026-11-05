# Feature: audit suppressions take effect only when saved, read fresh lines, and are honoured wherever findings are shown or counted

## Invariants

**INV-1 — each run reads source lines fresh.** `AuditDialog::runAudit` clears
`m_fileLineCache` before the run starts, so an inline suppression added since
the last run is seen.

**INV-2 — a suppression that was not saved does not hide the finding.**
`AuditDialog::saveSuppression` checks the rewrite's `write` and `commit()` and
the append's writes and flush. Only when the save succeeded does it add the
key to `m_suppressedKeys`; otherwise it reports the failure and returns
false.

**INV-3 — the Review-with-Claude export holds no suppressed finding.**
`AuditDialog::plainTextResults` skips every finding `isSuppressed` reports,
and counts each check's total and corroborated tag from what is left.

**INV-4 — a line-above `cppcheck-suppress` or `nosemgrep` is honoured.**
`AuditDialog::inlineSuppressed` passes a previous-line comment carrying either
marker to `AuditEngine::commentSuppresses`, as it does `NOLINTNEXTLINE`.

**INV-5 — suppressed findings are not counted as shown ones.**
`AuditEngine::capFindings` caps only unsuppressed findings and keeps the
suppressed ones (the SARIF export reports them). `AuditDialog::renderResults`
counts the findings this run's suppressions hid for its noise floor, not the
keys in `.audit_suppress`.

**INV-6 — a failed suppression save stops there.** `saveSuppression` returns
whether the save worked, and its caller returns on false, before the
learned-false-positive entry and the "Suppressed" status.

**INV-7 — an old-format baseline is not read as current (ANTS-6385).**
`AuditDialog::loadBaseline` checks the file's `version` before loading any
key; a file that is not version 2 is ignored and the status line asks for a
new baseline.

## Rationale

`inlineSuppressed()` reads source lines through `m_fileLineCache`, which only
`renderResults` cleared, so a run reused the previous run's lines.
`saveSuppression` ignored `QSaveFile::commit()` and the append's write results,
then marked the key suppressed regardless.

## Test surface

`test_audit_suppression_save.cpp` reads the AuditDialog sources and checks the
function bodies named above; INV-5's cap half runs `AuditEngine::capFindings`
on a synthetic result. The existing
`audit_sarif_suppressions` and `audit_dedup_96bit` checks on the same bodies
still hold.

## Regression history

- **ANTS-5083:** the two defects above. Locked by this spec.
- **ANTS-6381..6384:** suppressed findings reached the Claude export, took
  cap slots and inflated the noise floor; line-above cppcheck/semgrep markers
  were ignored; a failed save still reported success. Locked by INV-3..6.
- **ANTS-6385:** a v1 baseline loaded as v2 and marked every finding new.
  Locked by INV-7.
