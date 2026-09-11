# audit_corroboration_shift — AuditEngine::applyCorroborationShift + applyCorroborationShiftAcross (ANTS-1111 / ANTS-5041)

## Invariants

- INV-1: when 2+ distinct CheckIds cite the same (file, line) within one
  call's input, every finding on that line is severity-promoted by 1 tier
  (clamped to Blocker).
- INV-2: when only 1 CheckId cites a (file, line) AND that
  checkId is in `noisyRules`, the finding is severity-demoted by 1 tier
  (clamped to Info).
- INV-3: when only 1 CheckId cites a (file, line) AND that
  checkId is NOT in `noisyRules`, severity is unchanged.
- Same checkId firing multiple times at one (file, line) does NOT
  count as cross-tool corroboration.
- Findings without (file, line) are skipped.
- INV-4: `applyCorroborationShiftAcross` applies the INV-1 promotion
  across the whole run, not per check. Two `CheckResult`s whose findings
  share a (file, line) but come from distinct checks both get promoted,
  even though neither `CheckResult`'s own findings list contains the
  other's finding. A single-tool finding elsewhere is untouched.
- INV-5: the INV-2 demotion still holds when findings from unrelated
  checks are combined into one run — a noisy-rule single-tool finding
  drops a tier, and a control finding from a clean rule in a different
  `CheckResult` is unaffected.
- INV-6: the shift runs once per completed audit run, never once per
  render.
  - INV-6a: `AuditDialog::renderResults` does not call
    `AuditEngine::applyCorroborationShift`. `renderResults` runs on
    every filter keystroke, pill toggle, sort toggle and AI verdict; the
    shift has no repeat guard, so calling it there drifts severity down
    further on every render.
  - INV-6b: `AuditDialog::cancelAudit` and the run-completion branch of
    `AuditDialog::runNextCheck` each call
    `AuditEngine::applyCorroborationShiftAcross` before their first call
    to `renderResults()` — the shift runs once, over every check's
    combined findings, at the moment the run finishes.

## Approach

INV-1 through INV-5 call `AuditEngine::applyCorroborationShift` /
`applyCorroborationShiftAcross` directly with synthetic `Finding` /
`CheckResult` values — no dialog, no I/O.

INV-6a and INV-6b are source-grep tests against `src/auditdialog.cpp`
(comment-stripped): they assert the shift call is absent from
`renderResults`, and present — ahead of `renderResults()` — in the two
run-completion paths. A grep is the only way to hold a "runs once, in
the right place" claim; a behavioural test can't observe how many times
a private method fired internally. If a fix routes the run-completion
call through a small `AuditDialog` helper instead of calling
`applyCorroborationShiftAcross` directly from `cancelAudit` /
`runNextCheck`, the helper's body must still contain the
`applyCorroborationShiftAcross(` call for the grep to hold — the pin is
on the engine call name, not on the calling convention.

## Regression history

ANTS-1111 introduced `applyCorroborationShift`, called once per
`CheckResult` from `renderResults`. ANTS-5041 found that scoping wrong
in two ways: every render (filter keystroke, pill toggle, sort toggle,
AI verdict) re-runs the shift over already-shifted severities with no
repeat guard, so a noisy finding drifts down a tier per render; and the
per-check scope means the INV-1 cross-tool promotion can never see a
corroborating finding that landed in a different check's `CheckResult`.
Fix: apply the shift once, over every check's findings combined, from
the run-completion path (`cancelAudit` / `runNextCheck`'s finish
branch) rather than from `renderResults`.
