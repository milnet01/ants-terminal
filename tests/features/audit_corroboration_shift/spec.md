# audit_corroboration_shift — AuditEngine::applyCorroborationShift (ANTS-1111)

## INVs

- INV-1: when 2+ distinct CheckIds cite the same (file, line),
  every finding on that line is severity-promoted by 1 (clamped to
  Blocker).
- INV-2: when only 1 CheckId cites a (file, line) AND that
  checkId is in `noisyRules`, the finding is severity-demoted by 1
  (clamped to Info).
- INV-3: when only 1 CheckId cites a (file, line) AND that
  checkId is NOT in noisyRules, severity is unchanged.
- Same checkId firing multiple times at one (file, line) does NOT
  count as cross-tool corroboration.
- Findings without (file, line) are skipped.
