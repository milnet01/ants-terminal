# mutation_probe — feature contract (ANTS-4398)

The mutate-and-watch-it-go-red loop as a verb. Several projects' `CLAUDE.md`
files mandate mutating an invariant before believing it is held, and nothing
served that loop: `focused_test` is ctest-only and does not mutate,
`invariant_check` reads specs and runs nothing. So every session hand-rolled
the same ~40 lines of bash — one reporting project three times in a single
session, this project six times in one evening.

## Invariants

| INV | Statement |
|---|---|
| INV-1 | The three INERT shapes are detected and reported as `inert`, never as a surviving mutant: `old` absent from the file, `new` equal to `old`, and an empty `old`. An inert mutation runs NO test. *Test:* `MutationProbe.Inv1InertShapes` |
| INV-2 | `parseCounts` reads pytest, ctest and gtest summaries; unrecognised output leaves both counts at `-1`, which is distinct from `0`. *Test:* `MutationProbe.Inv2CountParsing` |
| INV-3 | The verb is registered on the WORKER delegate, verifies `restored_clean` rather than assuming it, refuses a red baseline under `require_green_baseline`, and executes `test_command` as argv with no shell path. *Test:* `MutationProbe.Inv3GuaranteesWired` |
| INV-4 | (ANTS-4521) A mutation may carry `expect_occurrences`. When it does not match how many times `old` occurs, THAT mutation is refused with outcome `occurrence_mismatch`, both counts reported, no write and no test run; the rest of the batch still runs. Absent ⟹ unchecked, and it does NOT default to 1 — a mutation meant to hit every site is legitimate. *Test:* `MutationProbe.Ants4521OccurrenceMismatchRefusesBeforeAnyTestRuns`, `…MatchingExpectationRunsNormally`, `…AbsentExpectationIsUnchangedBehaviour` |

## Why `inert` is the field that matters

From outside, an inert mutation and a surviving mutant are indistinguishable
— both end with a green test run. The wrong reading is "my test is weak" when
the truth is "my patch never applied". One session hit three in a row: a
comment-only edit, a `[... for x in []]` no-op, and a half-applied two-part
`sed`. Each initially read as "the suite holds this", and each was false.

So an inert mutation skips the test run entirely rather than running one and
disclaiming it. A run there would pass against unmutated code, which is the
false conclusion the verb exists to prevent.

## Why a mismatch refuses rather than warns (ANTS-4521)

`inert` guards a mutation that changed **less** than the caller believes. The
same defect runs the other way and was unguarded: a LocalWebServerManager
session meant to clear `high_contrast` on ONE palette, the literal occurred
twice, both were cleared, and the result came back `occurrences:2
outcome:killed`.

Nothing wrong followed because it died — and the dangerous direction is subtler
than survival. With N sites mutated, the mutant can be killed by a test
covering a site the caller never meant to touch while the site they DID mean to
probe stays uncovered. The verdict reads `killed`, the label — which is what
gets quoted in a commit message as evidence — describes a narrower mutation
than the one that ran, and the uncovered site is invisible. A false GREEN in a
verb whose whole purpose is refusing false greens.

`occurrences` was already reported, so the information existed; it arrived as
one integer among ten rather than as a check against intent. The refusal lands
before the write and before the run, and therefore before a verdict exists to
be misread — which is why it beats the weaker alternative of warning whenever
`occurrences > 1`.

## Why `test_command` is argv and not a shell string

This verb writes to a source file and spawns a process. A shell string would
make it an arbitrary-command surface reachable from a tool call. Everything a
test selector needs is expressible as argv, so the narrowing costs nothing.

## What the loop buys

The reporting session found **7 tests that were green and measured nothing**
across 58 mutants: a launcher that ignored SIGTERM, two fixtures already in
sorted order for a sort-order test, a session-global object count, a one-row
fixture against a per-row closure bug, and no fixture reaching two whole enum
states. Reading found none of them.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| impl | 2026-08-14 | — | — | — | — | — | Written alongside the implementation; no reviewer dispatched. |
