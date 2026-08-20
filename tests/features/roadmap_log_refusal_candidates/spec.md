# roadmap_log_refusal_candidates — feature-conformance contract

Locks two refusal affordances on `roadmap_log`, filed independently by two
sessions with the same complaint: **the refusal names no route forward.**

- [ANTS-4556] `bad_section` refuses an unknown slug and offers nothing, where
  three sibling refusals now rank near-misses — `read_region` section mode on
  both `section_ambiguous` (ANTS-2234) and `section_not_found` (ANTS-4350), and
  `apply_edits` on `not_found` (ANTS-4418) and `ambiguous` (ANTS-4473). The
  reported cost was a ~2.4 KB body discarded with no route but a
  `section_index` call answering 141 slugs.
- [ANTS-4574] `bullet_ambiguous` advises "narrow with anchor or id" when the id
  **is** what the caller passed and the matched bullets carry no anchor. Every
  route the message offers is closed.

Behavioural, against a real `ROADMAP.md` in a `QTemporaryDir`, driven through
the `…ForTest` seams. The subject is what the envelope *carries*, so a grep
over the refusal string would assert that the message looks like itself.

## What each case locks

| Case | ID | Breaks when |
|---|---|---|
| `BadSectionCarriesRankedCandidates` | 4556 | `bad_section` returns without `candidates[]`, or the list is emitted in store order rather than ranked, so a near miss (`performance` vs `performance-2`) does not surface first |
| `BadSectionCandidatesAreCapped` | 4556 | the cap is dropped and a 141-slug roadmap pays a full slug dump on every typo — the refusal body then costs more than the `section_index` call it replaces |
| `BadSectionStillRefuses` | 4556 | candidates are added by RESOLVING the near miss — the write must still refuse, because guessing which section the caller meant is how a body lands in the wrong place |
| `BadCaseStillWinsOverCandidates` | 4556 | the candidates path swallows a pure case mismatch, which has an exact answer (`canonical_slug`, ANTS-1524) and must not be downgraded to a ranked guess |
| `AmbiguousIdNamesTheHeadlineRoute` | 4574 | the message still says "narrow with anchor or id" after an **id** was the ambiguous locator, or it names `anchor` when the matched bullets carry none |

## Verifying RED

Per the project convention each case is shown failing against its *Breaks when*
mutation before the implementation is restored. Restore files with `write_text`
and never `shutil.copy2`: `copy2` preserves mtime, ninja then skips the
rebuild, and the mutation accumulates silently in a green-linking binary.

## Deliberately not locked

The **ranking function** is `ReadRegion::rankSectionCandidates`, already locked
by `read_region`'s own suite (ANTS-4350). This contract asserts that
`roadmap_log` *reaches* it and that the near miss surfaces — not how the score
is computed. Re-asserting the algorithm here would be a second copy of that
test which drifts from it.
