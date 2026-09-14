# Feature: audit auto-fix and the per-check cap respect current state

## Invariants

**INV-1 — auto-fix asks whether a finding is suppressed now.**
`AuditDialog::runAutoFix` skips a finding through `isSuppressed(f)`, not the
`f.suppressed` flag cached when the run parsed it.

**INV-2 — auto-fix reads only files it could repair.** `runAutoFix` skips a
finding with no line before reading its file, and does not read a file larger
than its size cap.

**INV-3 — an uncapped lane keeps every finding.** `handleCheckOutput` applies
`capFindings` only when the check's `filter.maxLines` is positive, so the
contract-doc drift lanes (`maxLines = 0`, ANTS-3600 INV-10) are not cut to the
per-check cap.

## Rationale

A suppression added after a run did not stop auto-fix from repairing the
finding. Auto-fix read every flagged file whole on the GUI thread before
`planRepair` decided whether it cared. ANTS-3600 uncapped the drift lanes'
output, but every check's findings were still cut at the per-check cap.

## Test surface

`test_audit_autofix_guards.cpp` reads `src/auditdialog.cpp` (located from the
test's own path) and checks the two function bodies.

## Regression history

- **ANTS-5083:** the three defects above. Locked by this spec.
