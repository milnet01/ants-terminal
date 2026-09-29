# `quotation_check` — ANTS-5502

**Status:** implemented (2026-09-29). Proven red against a stub `run()` first: 11 of 12 failed.

The contract is [`docs/specs/ANTS-5502-quotation-check.md`](../../../docs/specs/ANTS-5502-quotation-check.md).
This file names the test that locks each of its invariants; it does not
restate them.

## Fixtures

- `vectors/<name>.subject` and `vectors/<name>.quote` — one check each.
- `vectors/expected.tsv` — `<name>` TAB `HIT|MISS|ERROR`, one row per
  vector, measured with `quotation-check.sh` at `~/.claude` commit
  `60fd0be`.

Every other fixture is built in a `QTemporaryDir` by the test itself.

## Tests

The test drives the seam in `src/quotationcheckverb.{h,cpp}` directly,
with the temporary directory as the project root.

| Invariant | Test |
|---|---|
| INV-1 | `QuotationCheck.Inv1VectorsMatchCommittedExpectations`; `QuotationCheck.Inv1ScriptAgreesWithExpectations` skips with a message where the script is not installed. |
| INV-2 | `QuotationCheck.Inv2OneResultPerItemInOrder` |
| INV-3 | `QuotationCheck.Inv3EachReasonIsNotRun` |
| INV-4 | `QuotationCheck.Inv4MaxBytesNeverTrimsFindings` |
| INV-5 | `QuotationCheck.Inv5SymlinkOutsideRootIsOutsideAllowed`, `QuotationCheck.Inv5AllowedGlobsMatchResolvedPath` |
| INV-6 | `QuotationCheck.Inv6RefReadsTheBlob` |
| INV-7 | `QuotationCheck.Inv7MissPointsAtNearestText` |
| INV-8 | `QuotationCheck.Inv8SubjectReadOncePerCall` |
| INV-9 | `QuotationCheck.Inv9Caps` |
| INV-10 | `QuotationCheck.Inv10Registration` |
