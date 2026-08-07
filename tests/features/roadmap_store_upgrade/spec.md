# roadmap_store_upgrade — the store's schema-upgrade ladder

**ANTS-3781.** Contract:
[`docs/specs/ANTS-3781-roadmap-store-schema-upgrade.md`](../../../docs/specs/ANTS-3781-roadmap-store-schema-upgrade.md).
This file is the test's own summary of what it holds; the spec is authoritative
and this restates only what the assertions need.

`RoadmapStore::createSchema()` had three arms where it needed four: newer than
this build (refused), equal (commit and return), and everything else (run the
DDL). A store *behind* the running binary fell into the third and died on
`table already exists` — a message naming neither version, against a store whose
only rebuild path is the export.

`applyUpgrades()` is the missing arm's engine. A **rung** is one version step
and the statements that reach it; the **ladder** is the rungs. The production
ladder was empty at `kSchemaVersion` 1 — there was no version below it to climb
from — which is why the function takes its ladder as an argument: a ladder
reachable only from production was one nothing could exercise until the first
bump. **ANTS-3815 made that bump** (`kSchemaVersion` 2, `project.source_format`)
and supplied the first rung; the argument still earns its keep, because INV-1's
test climbs past `kSchemaVersion` with a ladder of its own.

## What each test holds

| Test | Invariant |
|---|---|
| `Inv1ClimbsAscendingAndStampsOnce` | INV-1 — one rung per version step in `(from, to]`, in **ascending version order** whatever order the ladder declares them in, and one `user_version` stamp after the last rung. Rung 3 references a column rung 2 adds, so declaration order and version order disagree observably. |
| `Inv2MissingRungRefusedBeforeAnythingRuns` | INV-2 leg (a) — a single-step climb with no rung. |
| `Inv2DuplicateRungRefusedBeforeAnythingRuns` | INV-2 leg (b) — two rungs landing on the same version. |
| `Inv2LaterMissingRungStopsTheEarlierOne` | INV-2 leg (c), **the leg that earns the invariant** — 1 → 3 with rung 2 present and rung 3 absent, asserting rung 2's effect is *not* present. Legs (a) and (b) are single-step, so a lazy per-rung lookup that validates as it goes passes both while breaking the "before any statement runs" clause. |
| `Inv3RungFailureLeavesNothingBehind` | INV-3 leg (a) — inside the caller's `BEGIN IMMEDIATE`, a two-statement rung whose second statement is invalid SQL; after the caller's `ROLLBACK` the first statement's effect is gone and the version is unmoved. |
| `Inv3bNoTransactionControlInApplyUpgrades` | INV-3 leg (b) — source scrape: no `BEGIN` / `COMMIT` / `ROLLBACK` / `SAVEPOINT` in `applyUpgrades()`'s executable statements. Not redundant with leg (a): leg (a) runs with a transaction already open, which is precisely when a stray `BEGIN` fails harmlessly and invisibly. |
| `Inv4ProductionLadderIsComplete` | INV-4 — every version in `[1, kSchemaVersion)` has exactly one rung landing one above it, and no rung's `to` falls outside `[2, kSchemaVersion]`. Was green and vacuous at `kSchemaVersion` 1 by construction; **ANTS-3815's bump made its loop range non-empty**, which is what it was written for, and it now checks a real rung with no edit of its own. |
| `Inv5CreatedSchemaStaysOnTheCreationPath` | INV-5 — `m_createdSchema` is assigned in exactly one place **and that place is on the creation path**. Both legs, because a count alone is green against the regression the invariant names: *moving* the single assignment onto the upgrade arm leaves the count at one. |
| `Inv6ExportNeverNamesTheStoresConstant` | INV-6 — zero occurrences of the store's table-version constant in either export source file, **comments included**. |
| `Inv7FromBelowOneRefusesLegibly`, `Inv7FailedStampRefusesLegibly` | INV-7's two refusals that no other test reaches. The other four are asserted in place, by the INV-2 and INV-3 tests. |
| `Inv8DdlBuiltAndClimbedStoresMatch` | INV-8 — **unrunnable until `kSchemaVersion` moves**, and written as a tripwire that reddens at the bump rather than a skip that stays quiet forever. |

## The delimiting rule the two source scrapes share

INV-3 leg (b) and INV-5 both grep a **function body**, never the file:
`createSchema()` legitimately contains `BEGIN`, `COMMIT` and `ROLLBACK`, and
after this change it is no longer the only function in `src/roadmapstore.cpp`
that stamps `PRAGMA user_version`. A whole-file grep is not merely imprecise
here, it is guaranteed wrong.

The rule: from the line matching the function's signature to the next line
whose **first character** is `}`. The file's existing style puts a closing brace
at column 0 and nowhere else. Line comments are stripped before INV-3 leg (b)
matches, because the natural body comment says "runs inside the caller's
`BEGIN IMMEDIATE`" and a comment-blind grep would redden a correct
implementation.

## Verifying RED first

Most of these cover a function that did not exist, so "fails against pre-fix
source" cannot mean a compile against it — and the obvious substitute (stub
`applyUpgrades()` to `return false`) does not work, because a stub that refuses
everything *satisfies* most of the clauses. Only **INV-1** (asserts both rungs
landed) and **INV-6** (the constant appears four times in today's export) have a
genuine pre-fix RED. INV-2, INV-3 and INV-7 are proved by **mutation** against
the finished implementation — each mutation is the invariant's own *Breaks
when:* clause, applied and reverted. INV-4 and INV-8 have no proof available at
all until `kSchemaVersion` moves, and are left that way deliberately: anything
that made them non-vacuous today would have to invent a version to climb.

Spec § 6 carries the per-invariant table and the results are recorded in the
spec's loop log.
