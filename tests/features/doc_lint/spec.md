# doc_lint — engine conformance

Contract for `tests/features/doc_lint/test_doc_lint.cpp`. Owning spec:
[`docs/specs/ANTS-3663.md`](../../../docs/specs/ANTS-3663.md).

Phase-1 rows only. The fix-path invariants belong to ANTS-3669 and to
`tests/features/doc_lint_fix/`, which does not exist yet.

**ANTS-5506 adds five engine rows for the sixth checker, `doc_facts`:**
INV-22 (`count_mismatch`), INV-23 (`invariant_duplicate`), INV-24
(`leaked_markup`), INV-25 (`version_drift`) and INV-27 (`verb_arg_unknown`).
`doc_facts`'s two verb-layer rows, INV-26 and INV-28, live in
`tests/features/doc_lint_verb/spec.md` — `cmdDocLint` needs a live
MainWindow-equivalent to resolve the version and the live verb schema, which
this directory's fixtures do not have.

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
| `Inv22CountMismatchNeedsBothSides` | INV-22 | `count_mismatch` fires only when a lead-in with exactly one cardinal is followed by a list whose top-level count differs; a two-cardinal lead-in, a non-list follow-on and a fenced span are none of them claims. Carries `count_claims_checked`. |
| `Inv22CountClaimIsOneSentence` | INV-22 | The claim is the lead-in's last sentence, and a number in it is a count only when it stands alone: a numbered item's marker, a label (`Phase 7`), a joined token (`loop-2`), a sum with `one`, and a wrapped paragraph's tail never claim; a parent list's next item and a change of marker kind end the counted list. One positive guard fires. Each case was a false hit on this repo's docs. |
| `Inv23DuplicateInvariantNamesBoth` | INV-23 | `invariant_duplicate` fires on the second of two bullet definitions of one id inside an Invariants section and names the first's line; a table row, a prose mention and a bullet under another heading are all excluded. |
| `Inv24LeakedMarkupIgnoresCode` | INV-24 | `leaked_markup` fires on a bare `invoke`/`function_results` tag and ignores the same tag inside an inline code span or a fence. |
| `Inv25VersionDriftReadsOnlyClaims` | INV-25 | `version_drift` fires only on a plain-text version claim that differs from the injected version; the same text inside backticks, a `CHANGELOG.md` file, and an empty `projectVersion` (which also sets `version_unavailable`) are all excluded. |
| `Inv27VerbArgsTopLevelOnly` | INV-27 | `verb_arg_unknown` checks only TOP-LEVEL keys of a known verb's `{…}`/`key:value` call against the injected map; a nested key, an unmapped verb name and a non-call span are not checked. An empty `verbArgs` sets `schema_unavailable` and disables the kind. Carries `verb_calls_checked`. |
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

**ANTS-5506's mutations, run 2026-09-28 against `src/docfacts.cpp`** (each
rebuilt, run, restored and byte-compared):

| Mutation | What it actually turned red |
|---|---|
| INV-22: a lead-in with two or more numbers still claims | `Inv22CountMismatchNeedsBothSides` (2 findings: (c) fired) |
| INV-22: count nested items | `Inv22CountMismatchNeedsBothSides` ((b) fired) |
| INV-23: match bullets under any heading | `Inv23DuplicateInvariantNamesBoth` (the Notes bullet fired) |
| INV-24: stop removing inline code spans before matching | `Inv24LeakedMarkupIgnoresCode` (3 findings: line 2 fired) |
| INV-25: drop the `CHANGELOG` exclusion | `Inv25VersionDriftReadsOnlyClaims` (`CHANGELOG.md:3` fired) |
| INV-27: descend into nested values (key reading AND the nested `{` opening, together) | `Inv27VerbArgsTopLevelOnly` (`bogus` fired) |
| INV-27: either half of that alone | **Nothing** — each half alone changes no behaviour; a mutant, not a fixture, gap |
| INV-22 refinements: drop the label rule / the standalone-token rule / `one` in the tally / the wrapped-tail rule / the parent-list break / the marker-kind break | `Inv22CountClaimIsOneSentence`, each one |
| INV-22 refinement: strip the list marker before counting | **Nothing** — redundant with the standalone-token rule, so the strip was removed |

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
