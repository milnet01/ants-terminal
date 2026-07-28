# spec_lint — engine conformance

Contract for `tests/features/spec_lint/test_spec_lint.cpp`. Owning spec:
[`docs/specs/ANTS-3662.md`](../../../docs/specs/ANTS-3662.md).

`SpecLint::check` is pure — document text in, findings out — so every row here
drives it directly, with no MainWindow and no filesystem. The one exception is
INV-1 arm (c), which reads the project's real `docs/standards/specs.md`
deliberately: the arm's whole point is that the corpus's live state is the thing
an implementation is tempted to guess around.

## What each row locks

| Row | Invariant | Claim |
|---|---|---|
| `Inv1SectionListIsReadNotAssumed` | INV-1 | The required-section list comes from a `<!-- required-sections -->` block; absent block → skipped, `sectionsChecked == false`. |
| `Inv2EveryInvariantNeedsATestClause` | INV-2 | An `INV-N` with no test surface fires once, in **both** spec forms; a tombstone does not fire. |
| `Inv3TombstoneIsNotAGap` | INV-3 | Only a genuinely missing number is a gap; both tombstone markers are exempt. |
| `Inv5LoopLogOutcomeCells` | INV-5 | A loop row with an empty **last** cell fires, in every table shape; a non-`Loop` table is not scanned. |
| `Inv6SizeIsNotAFinding` | INV-6 | Size is reported in the envelope and never emitted as a finding. |
| `DISABLED_CorpusCalibration` | — | § 2.1's fire-rate measurement for `command_test_no_expectation`. Not a contract; re-runnable. |

## Verified RED before the implementation landed

Three rows assert an **absence** of findings and would pass vacuously against an
engine that finds nothing. Each was re-proven by mutating the shipped engine and
recording what the mutation actually turned red — a compile failure proves
nothing here, because the exclusion arms pass against an engine that harvests
nothing at all.

| # | Mutation | Result |
|---|---|---|
| M1 | Default `requiredSections` to a guessed list instead of skipping | RED — arm (b) and arm (c) both fail: `sectionsChecked` becomes true and `missing_section` fires against a conforming doc. |
| M2 | Drop the `isTombstone` guard | RED — `Inv2` reports `INV-3` (both forms) and `Inv3` reports 3 findings where 1 is expected. |
| M3 | Emit `line_count` as a `doc_size` finding | RED — `Inv6`'s first arm fails on the kind-name assertion, second arm on `findings.isEmpty()`. |
| M4 | Build the id list from the parser alone, dropping the anchor scan | RED — `Inv2`'s two-column arm reports 0 where 2 are expected. **Survived the first run**; see below. |
| M5 | Key the loop table on an `Outcome`-named column instead of the last cell | RED — `Inv5` reports 1 where 2 is expected: the 6-column shape has no such column. |

Every row above was re-run after the fixtures were sharpened and after the
`parseSpecBody` fix (ANTS-3683), because the code under mutation had changed and
an old RED proves nothing about new code.

**M4 survived its first run, and the diagnosis is worth keeping.** The fixture
at the time was a three-column table whose surface cell was blank, and the claim
was that `parseSpecBody` drops such a row. It does not — its lazy three-cell
regex captures the whitespace as a present-but-blank `test_surface`, so the
parser returned the row and the anchor scan was doing no work the test could
see. Two things came out of that:

1. The engine's real guard is that **blank counts as absent**, not that the
   anchor scan supplies the id. Both are needed, for different shapes.
2. The shape the parser genuinely cannot see is a **two-column** table — no test
   column at all — and that is now the fixture. It is also the more valuable
   case: a spec with no test column is malformed in exactly the way this check
   exists to report.

Chasing that down found **ANTS-3683**: `parseSpecBody`'s row regex separated
cells with `\s*`, and `\s` matches a newline, so on that same two-column table
one match ran across two rows — returning INV-1 with a `test_surface` of
`| INV-2 | another`, and consuming INV-2 out of the list entirely. A well-formed
three-column table masked it. Fixed in `specparse.cpp`; this row is its
regression lock.

## Not covered here

- `command_test_no_expectation` — the verb-lane contract owns it
  (`tests/features/spec_lint_verb/`), because its "runs no subprocess" half is a
  source scrape rather than a behavioural assertion.
- The loop-log **balance** check — dropped, with the measurement, in the owning
  spec's § 5. `ANTS-3682` carries the prerequisite.
