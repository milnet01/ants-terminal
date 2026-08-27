# spec_lint — engine conformance

Contract for `tests/features/spec_lint/test_spec_lint.cpp`. Owning specs:
[`docs/specs/ANTS-3662.md`](../../../docs/specs/ANTS-3662.md) and
[`docs/specs/ANTS-4127-test-surface-resolution.md`](../../../docs/specs/ANTS-4127-test-surface-resolution.md).
Rows are prefixed by owner: `Inv*` are ANTS-3662's, `Ants4127Inv*` are
ANTS-4127's, and the two numbering schemes are independent.

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

### ANTS-4127 — test-surface resolution

| Row | Invariant | Claim |
|---|---|---|
| `Ants4127Inv1SurfaceHarvest` | INV-1 | One attempt per **distinct** name; a wrapped clause harvested whole; distinctness scoped **per document**. |
| `Ants4127Inv2WildcardIsNotADirectory` | INV-2 | `tests/features/audit_*` names no directory — no attempt, no finding — while a real name in the same clause still resolves. |
| `Ants4127Inv3StatusChoosesTheBucket` | INV-3 | Two documents differing **only** in their Status line split into `test_surface_absent` and `test_surface_unresolved`. |
| `Ants4127Inv4UnparsedStatusNeverReachesFinding` | INV-4 | No Status line, or an unrecognised word, is a CANDIDATE; the three normalisation steps each get an arm. |
| `Ants4127Inv5AbandonedSpecsAreSkippedEntirely` | INV-5 | `superseded` / `considered` skip **before either check**: nothing emitted, and `surfacesResolved == 0`. |
| `Ants4127Inv6UnwiredIsStatusProof` | INV-6 | Present-but-unwired is its own kind and fires whatever the live Status. |
| `Ants4127Inv7ResolvedCountsSurfacesNotClauses` | INV-7 | The counter is over surfaces, not clauses, and zero is a reported value. |
| `Ants4127Inv8EngineTouchesNoFilesystem` | INV-8 | Comment-stripped token scrape over `speclint.{h,cpp}`, plus a corpus digest either side of a real run. |
| `Ants4127Inv9EmptySetMeansSkipNotFail` | INV-9 | Each set gates its own check; empty means skip. Arms (a) and (b) differ **only** in whether the set was empty. |
| `Ants4127Inv10CheckedIsNotInferredFromTheCounter` | INV-10 | `surfacesChecked` is false exactly when INV-9 skipped. **Arm (b) is the only falsifier.** |
| `Ants4738PrefixMatchIsOptInOnTheBlock` | ANTS-4738 | A numbered required entry matches on its `## N. Name` prefix, so a trailing qualifier passes — but only when the standard's marker asks for it. Verbatim stays the default, an absent section is still reported under the flag, and a prefix ending mid-word is not a match. |
| `Ants4739DocumentCanExemptItselfFromRequiredSections` | ANTS-4739 | A document carrying the exemption marker outside fenced code reports no `missing_section`; `sectionsChecked` stays true, and the marker inside a fence does not fire. |

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

### ANTS-4127 — verified RED

Five of the ten rows assert positive counts of a `test_surface_*` kind and so
fail against pre-implementation source by construction: those kinds did not
exist. The rows that assert an **absence** would pass vacuously, and each was
re-proven by mutating the shipped engine.

| # | Mutation | Result |
|---|---|---|
| M1 | Drop the `(?!\|w\|\*)` lookahead from the extraction pattern | RED — `Inv2` reports a directory named `audit_` absent, filing a finding against a correct spec. |
| M2 | Harvest regardless of `existingTestDirs` being empty | RED — `Inv9` arm (a) reports 1 where 0 is expected: an empty set condemning everything it reads. |
| M3 | Apply the abandoned-status skip per-CHECK (absence only) instead of per-spec | RED — `Inv5`'s present-but-unwired arm fires on a `superseded` spec. Its absent arm stays green, which is why the unwired arm is in the fixture. |
| M4 | Add a `QDir` to `speclint.cpp` | RED — `Inv8`'s token scrape. Confirms the scan survives comment-stripping rather than being satisfied by it. |
| M5 | Infer `surfacesChecked` from `surfacesResolved > 0`, leaving the gate itself correct | RED — `Inv10` arm (b) **only**; arms (a) and (c) pass, which is exactly the claim § 2.3 makes about them. `Inv7`'s zero-with-checked assertion goes red on the same defect. |

**M5 was cut twice, and the first cut is the lesson.** Removing the
`surfacesChecked` assignment outright also disabled the harvest gate that reads
it, so nine rows went red and the mutation proved only that the feature was
switched off. A mutation that turns everything red demonstrates nothing about
the row it was aimed at. The second cut kept the gate on a local and mis-derived
only the reported flag — one row, the intended one.

The hash arm of `Inv8` is **not** mutation-proven: writing an engine that writes
in order to watch the digest move would damage the corpus it digests. It is a
guard, and recorded as one.

## Not covered here

- `command_test_no_expectation` — the verb-lane contract owns it
  (`tests/features/spec_lint_verb/`), because its "runs no subprocess" half is a
  source scrape rather than a behavioural assertion.
- The loop-log **balance** check — dropped, with the measurement, in the owning
  spec's § 5. `ANTS-3682` carries the prerequisite.
