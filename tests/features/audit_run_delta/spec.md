# audit_run since-last-run findings delta (ANTS-1870)

Behavioural contract for the precise since-last-run delta + carry-forward
SARIF merge. Full design: `docs/specs/ANTS-1870.md`.

## Invariants under test

- **INV-1** — `parseToolOutput` populates the full `findings[]` set (every
  non-suppressed finding, up to `kSarifFindingsMax`), each carrying a
  16-hex `fp`, while `samples` keeps its `sampleCap` preview. Exercised via
  the `internal::parseWithSuppression` hook (`findingsCount` /
  `sampleCount` / `allFindingsHaveHexFp`).
- **INV-2** — Finding identity (`computeFingerprint(file, checkId,
  message)`) is line-insensitive: the same finding at a shifted line has an
  equal `fp`, so it classifies as carried-forward, not added+removed.
- **INV-3** — `computeDelta` partitions added / removed / carried_forward /
  merged correctly, including the degenerate inputs (empty current, empty
  prior, both empty).
- **INV-4** — The findings sidecar round-trips through `recordRun` +
  `loadFindingsSidecar`; the basename iso is hyphen-form while the body
  `iso_timestamp` carries colons; a `version:2` sidecar loads empty.
- **INV-5** — History reaping deletes dropped runs' `findings-*.json`
  sidecars alongside the SARIF/HTML.
- **INV-6 / INV-7 / INV-8 / INV-10** — runAudit + envelope wiring
  (carry-forward SARIF, chain integrity, honest fallback, envelope
  mutual-exclusion) — source-scrape, the `audit_run_since_last_run`
  pattern.
- **INV-9** — `computeDelta` + the delta module are pure (no `QProcess`,
  no filesystem) — source-scrape of `auditdelta.cpp`.

## Out of scope

Cross-tool correlation, line-precise identity, AuditDialog GUI delta
rendering, `last_audit_summary` delta echo (see spec § 5).
