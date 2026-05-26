# `spec_query` + `project_layout` phases-dir recognition — test contract

Locks the invariants in `docs/specs/ANTS-1880.md`.

## Anchors

| INV | Test                                          | What it checks |
|-----|-----------------------------------------------|----------------|
| INV-1 | `Inv1IdValidatorAcceptsBothShapes`          | `isValidSpecId` accepts ANTS-NNNN AND `^phase_[0-9]+_[a-z0-9_]+$`. |
| INV-2 | `Inv2NoCrossDirFallback`                    | Per-id-shape routing: ANTS-NNNN → docs/specs/; phase_* → docs/phases/. |
| INV-3 | `Inv3SourceFieldEchoed`                     | Response carries `source` field with "specs" or "phases". |
| INV-4 | `Inv4LayoutEnvelopeCarriesPhasesDir`        | LayoutEnvelope has phasesDir field + phases_dir JSON key. |
| INV-5 | `Inv5ProbeSetVersionBumped`                 | kProbeSetVersion = 4. |
| INV-6 | `Inv6InvariantCheckScansBothDirs` + sib     | cmdInvariantCheck walks both dirs; phases_scanned + total_scanned added; specs_scanned semantics preserved. |

## Pre-fix verification

Before the fix: `kProbeSetVersion = 3`, no `phases_dir` literal,
no `^phase_` regex in isValidSpecId, no `phasesDir` field.
After the fix: tests turn GREEN.
