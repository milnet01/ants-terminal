# doc_lint — the fix path (ANTS-3669)

Phase 2 of `doc_lint`. The read half ships in ANTS-3663; this directory covers
everything behind `fix:true` — the only part of the doc-lint family that writes
to a file.

The contract is `docs/specs/ANTS-3663.md` § 2.3. This file records how the rows
are tested and what each mutation actually turned red.

## Rows

| INV | Test | Asserts |
|---|---|---|
| INV-5 | `Inv5OnlyAutoFixableAndTheTwoCounts` | only a flagged finding is repaired; `fixed` counts findings and `files_written` counts files |
| INV-5 | `Inv5GateIsTheFlagNotTheKind` | the engine gates on `autoFixable`, not on `kind` |
| INV-6 | `Inv6ReportOnlyByDefault` | a default run writes nothing, and the fix keys are absent rather than zero |
| INV-12 | `Inv12DryRunMatchesRealEnvelope` | the preview equals the run it previews, bar the `dry_run` echo |
| INV-14 | `Inv14WriteFailureIsContained` | a failed write reports nothing repaired and leaves the original intact |
| INV-15 | `Inv15CapDoesNotBoundRepairs` | `max_findings` pages the findings and never the repairs |
| INV-16 | `Inv16DryRunRequiresFix` | `dry_run` without `fix` refuses `bad_args` |
| INV-18 | `Inv18TocRepairIsAPatch` | an H3 entry, a free-text row and an entry matching no heading all survive |
| INV-18 | `Inv18NoTemplateRefusesRatherThanGuessing` | a region with no top-level entry to copy is not repaired |
| INV-18 | `Inv18DuplicateEntryIsDeletedKeepingTheFirst` | the other `toc_gap` cause repairs, keeping the first occurrence |
| INV-21 | `Inv21StaleDocumentIsRefusedWhole` | a document that changed under the walk is refused whole |
| INV-21 | `Inv21StaleDuplicateDoesNotDeleteAnInnocentLine` | the staleness check itself, not merely its outcome |
| INV-21 | `Inv21ShiftedLinesStillRepair` | a gap whose lines moved still repairs, from the file rather than the walk's text |

## Fixture choices, and why each is what it is

**The mid-run seam.** § 6 requires the harness to expose one, because nothing
inside a single call can produce the window the precondition is about — the
walk has finished, the fix path has opened nothing, and the user saves. It is
`Options::afterWalkHook`, test-only alongside `Probe`.

**INV-18's document carries all three row types a regeneration would eat.** An
H3 child entry, a free-text row, and an entry whose slug matches no heading. A
TOC with only the first two cannot tell a patch from a regeneration, which is
the only thing the row asserts.

**INV-14 makes the DIRECTORY unwritable, not the file.** `QSaveFile` writes a
temporary beside the target and renames, so a read-only file is not refused.
Permissions are restored before the assertions run — `QTemporaryDir` cannot
clean up a directory it may not write.

**INV-21 needs a DUPLICATE gap, and the missing-section arm is not enough.**
This was measured, not reasoned: deleting the staleness check left the
missing-section row green, because the patcher's own lookup finds no uncovered
heading and refuses on its own. Two guards, one outcome. The duplicate branch
deletes by line number, so without the check it removes whatever now occupies
the reported line — which is real data loss and a fixture can see it.

## Mutation results

Every row here asserts an absence, and each passed on its first run against an
engine that repaired nothing — the vacuous state § 6 predicts. Each was re-proven
by deleting the rule under test.

| Mutation | Result |
|---|---|
| gate the fixer on `kind == "toc_gap"` instead of on `autoFixable` | **RED** — `Inv5GateIsTheFlagNotTheKind` |
| make the write path ignore the `fix` flag | **RED** — `Inv6ReportOnlyByDefault` |
| make `dry_run` skip appending to `fixed[]` | **RED** — `Inv12DryRunMatchesRealEnvelope` |
| append to `fixed[]` on intent rather than on success | **RED** — `Inv14WriteFailureIsContained`, and four more |
| delete TOC entries whose slug matches no heading | **RED** — `Inv18TocRepairIsAPatch` |
| page `fixed[]` with `max_findings` | **RED** — `Inv15CapDoesNotBoundRepairs` |
| delete the `dry_run`-without-`fix` guard | **RED** — `Inv16DryRunRequiresFix` |
| accept a stale document | **SURVIVED first, then RED** — see below |

**The staleness mutation survived its first fixture, and that is recorded rather
than tidied away.** With `if (stale)` deleted the suite stayed green, because
the only stale fixture at the time was a missing section, which the patcher
refuses on its own grounds. The response was to sharpen the fixture — a
duplicate gap, where the two guards give different answers — never to soften the
claim. It reddens now.

## Not covered here, deliberately

- **`read_failed` as a fix error** has no row. It needs the file to vanish
  between the walk and the write, which the seam can drive, but the outcome is
  byte-identical to `stale` from the caller's side: nothing written, the run
  continues, `ok` stays true. A fixture would assert the reason string alone.
- **A short write** is unreachable without a filesystem that accepts an open and
  then truncates. The `write_failed` reason is covered through the unwritable
  directory instead, which reaches the same branch.
- **Phase-1 rows** live in `tests/features/doc_lint/` and
  `tests/features/doc_lint_verb/`. Their mutation results are in those
  directories, including the one that reddened nothing.
