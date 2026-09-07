# doc_lint — engine conformance

Contract for `tests/features/doc_lint/test_doc_lint.cpp`. Owning spec:
[`docs/specs/ANTS-3663.md`](../../../docs/specs/ANTS-3663.md).

Phase-1 rows only. The fix-path invariants belong to ANTS-3669 and to
`tests/features/doc_lint_fix/`, which does not exist yet.

`DocLint::run` is filesystem-shaped by nature: two of its five checkers are
frozen engines that re-read the document themselves, so a seeded temp tree is
the only honest fixture. The root comes from `../doc_citations/fixture.h`
rather than a local copy — that header states why, and a divergent copy would
make these rows pass or fail for reasons unrelated to `doc_lint`.

## What each row locks

| Row | Invariant | Claim |
|---|---|---|
| `Inv1SharedReadForNativeCheckers` | INV-1 | One open per document serves all three native checkers. Every document sits under `specs_dir` so `spec_lint` is eligible for each; otherwise the counterfactual changes and the number stops discriminating. |
| `Inv3OkCitationIsNeverAFinding` | INV-3 | An `ok` citation is never a finding, including one whose anchor moved. |
| `Inv4UnparsedIsNeverAFinding` | INV-4 | `unparsed[]` entries never become findings, and are not lost either. |
| `Inv7TotalDocumentOrder` | INV-7 | Two runs agree key for key, the tied pair is in ascending `emissionIndex`, and every adjacent pair is ordered by the stated keys. |
| `Inv9WholeDocumentAlarmsSurvive` | INV-9 | The two unterminated-region alarms become findings carrying the opener's line; `examples_suppressed` becomes a statistic and never a finding. |
| `Inv9IncompletenessRoutesToCheckErrors` | INV-9 | An incomplete citation set routes to `check_errors[]`, not `check_stats`, and the checker stays in `checks_run[]`. |
| `Inv10CheckerFailureIsContained` | INV-10 | Containment on both axes: per document and per checker, with the walk completing. |
| `Inv17EligibilityFiltersWithoutSkipping` | INV-17 | An ineligible document is not a skip and not an error; a run selecting no spec leaves `spec_lint` out of `checks_run[]`. Carries the `line_count` map-merge assertion. |
| `Inv19UncheckedDocumentsAreNamed` | INV-19 | Every unchecked document is named with its reason, and the result says it is incomplete. |
| `Inv20CitationFilesUnderItsDocument` | INV-20 | A citation finding is filed against the document that contains it; the target appears only in `message`. |
| `DISABLED_CorpusCalibration` | — | Not a contract. Prints the figures § 6 asks for, measured against the real docs tree, so a later sweep can re-measure rather than trust a pasted number. Re-runnable. |

## Two fixture choices worth stating

**The unreadable document is a DIRECTORY**, not a `chmod 000` file. `QFile::open`
fails on it either way, and the directory works identically for a root and a
non-root runner — a permission bit does not.

**INV-7's tied pair uses identical link text AND an identical target**, so the
two findings agree on every serialised key. Two links differing only in their
text would differ in `message`, and the comparator would separate them on the
fifth key rather than needing the sixth.

## Must fail first — what each mutation actually turned red

Every row here asserts an absence or a routing, so each passes vacuously
against an engine that produces nothing — the state they first ran in. Each was
therefore re-proven by deleting the rule under test.

| Mutation | Turned red |
|---|---|
| Promote an `ok` citation with `anchor_found:false` to a finding | `Inv3OkCitationIsNeverAFinding` |
| Adapt `unparsed[]` entries into findings | `Inv4UnparsedIsNeverAFinding` |
| Emit `examples_suppressed` as a finding | `Inv9WholeDocumentAlarmsSurvive` |
| Route the citation-set incompleteness to `check_stats` instead of `check_errors[]` | `Inv9IncompletenessRoutesToCheckErrors`, `Inv10CheckerFailureIsContained` |
| Drop the eligibility predicate and dispatch every checker at every document | `Inv17EligibilityFiltersWithoutSkipping` |
| List a cap-elided document without setting the truncation flag | `Inv19UncheckedDocumentsAreNamed` |
| File a citation finding under the target path instead of the document | `Inv20CitationFilesUnderItsDocument`, plus `Inv9WholeDocumentAlarmsSurvive` and `Inv10CheckerFailureIsContained` as collateral |
| Count an open per checker rather than per document | `Inv1SharedReadForNativeCheckers` |
| **Drop the `emissionIndex` tiebreak from the comparator** | **Nothing.** |

### The surviving mutation, and why it is not a weak fixture

`DocLint::run` sorts with `std::stable_sort`, so a tie already keeps insertion
order — and findings are appended in emission order, so insertion order *is*
ascending `emissionIndex`. No fixture over this implementation can redden that
mutation: the two orderings cannot be made to disagree.

The key is kept anyway, and the claim is not softened. It is what makes the
order total rather than merely deterministic, and it is the guard if the sort is
ever changed to an unstable one — at which point the mutation becomes reddenable
and this row starts earning its place.

## What the calibration is for

Its figures are deliberately NOT copied into this document — a pasted number
goes stale silently, and the whole point is that a later reader can re-run it.
Two things its first run established that no fixture could. One open per
document holds at corpus scale, not just over the three-file INV-1 tree. And the
inherited symbol budget is exhausted early in a whole-corpus run, leaving most
needles `not_checked` — reported honestly through the tri-state and the
truncation flag, but thin enough to be worth a decision; ANTS-4917 carries it.

## Not covered here, deliberately

`check_errors[]`'s `check_failed` reason has no row. It means a checker
*produced nothing* for a document — it refused, threw, or hit an internal cap —
and the engine's current call shape cannot reach it: every checker is handed
text that the shared read has already validated, so a checker that would refuse
the document was never given it. A fixture for an unreachable state passes
without testing anything. Named here so a later reader finds a decision rather
than an omission, in the same posture ANTS-3663 § 3 takes toward
`basename_index_truncated`.
