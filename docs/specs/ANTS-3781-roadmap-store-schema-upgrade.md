# ANTS-3781 — Give `RoadmapStore` a schema-upgrade path

**Status:** spec draft (2026-08-07).
**Kind:** fix.
**Source:** ROADMAP.md ANTS-3781 (in-session-2026-08-01; the gap
[ANTS-3756](ANTS-3756-roadmap-store-schema.md)'s own Status line names as
unclosed).
**Blocker for:** ANTS-3815 (the first schema change that needs a rung),
ANTS-3860 (the export-side half of the same problem).
**Composes with:** [ANTS-3756](ANTS-3756-roadmap-store-schema.md) § 2.3, whose
version contract this completes.

**Layman:** The roadmap database stamps a layout number on itself. If it ever
meets a newer build, nothing knows how to bring it up to date — it refuses to
open, with an error that never says why. This adds the missing "bring it up to
date" step, and stops a database change from spoiling the backup files at the
same time.

## 1. Problem

`RoadmapStore::createSchema()` (`src/roadmapstore.cpp`) branches on the version
it reads inside its `BEGIN IMMEDIATE`, and has three arms where it needs four:

1. `version > kSchemaVersion` — refused, naming both numbers.
2. `version == kSchemaVersion` — commits and returns.
3. **everything else** — falls through to the DDL. There is no `version == 0`
   test; creation *is* the fall-through, which is why what follows is a gap
   rather than a branch someone left unfilled.

A version strictly between 0 and `kSchemaVersion` therefore reaches arm 3. The
DDL is written **without** `IF NOT EXISTS`, and the reason is one step removed
from where that fact is usually quoted:
[ANTS-3756](ANTS-3756-roadmap-store-schema.md) INV-15's discriminator is the
`user_version` read inside `BEGIN IMMEDIATE`, **not** the absent
`IF NOT EXISTS`. Its loop log row `8-impl` says so directly — "`user_version`
already guarantees the loser never reaches the DDL, so a `CREATE TABLE` that
runs against an existing table means the discriminator has regressed and must
fail loudly" — and INV-15's own *Breaks when* clause names gating on
`IF NOT EXISTS` as the failure, not the mechanism. The absent `IF NOT EXISTS`
is what makes that regression **loud**. The effect on an older store is the
same either way, and it is the wrong one: it is not re-created, it dies on
`table already exists`. Three consequences:

1. There is no route from an older store to a current one at all. Its only
   other recovery route is the export, and ANTS-3756's own header comment
   (`src/roadmapstore.h`) states the store is **primary**: "its only rebuild
   path is the export".
2. The failure is illegible. `table already exists` names neither version, so
   the one message a user would act on is the one they do not get.
3. Nothing stops a bump landing without its migration. `kSchemaVersion` is a
   constant a future change edits; the rung that goes with it is a separate
   edit nobody is holding.

Still unreachable **today**, and that is measured rather than assumed:
`kSchemaVersion` is 1, so no store below it can exist. It stops being
unreachable at ANTS-3815, which is the first change that bumps it, and a
version-1 store file exists on this machine now.

**A fourth problem, handed here by a sibling spec.**
[ANTS-3796](ANTS-3796-section-record-completeness.md) § 2.4 records that the
export's `meta.schema` value and the store's `user_version` are **one
constant** and that "separating them is ANTS-3781's problem, not this spec's".
They describe different things — the JSONL record shape versus the table shape
— and while they are welded together, *any* store-side bump invalidates every
export file ever written, including three committed goldens. That is the
mechanism behind ANTS-3860, and unwelding the constants is most of its
mitigation.

Reference counts, `grep -rn 'kSchemaVersion' src/ tests/ --include=*.cpp
--include=*.h` filtered to the roadmap files: `src/roadmapstore.h` 1,
`src/roadmapstore.cpp` 5, `src/roadmapexport.cpp` 4 (three live references and
one comment), `tests/features/roadmap_store_schema/` 3,
`tests/features/roadmap_store_concurrency/` 1.

## 2. Surface

### 2.1 The ladder

A rung is a version step and the statements that reach it. All three new
symbols are **public on `RoadmapStore`**, declared in `src/roadmapstore.h`:

```cpp
// One rung of the upgrade ladder: the statements that take a store from
// version `to - 1` to version `to`, in the order given.
struct Upgrade {
    int to;
    QStringList statements;
};

// Climbs `from` to `to`, applying exactly one rung per version step, and
// stamps `PRAGMA user_version = to` once after the last rung.
//
// TWO PASSES, and the split is the contract rather than an implementation
// detail (INV-2). Pass one validates the WHOLE range and executes nothing:
// every version in (from, to] must have exactly one rung, and any other count
// refuses naming that version. Pass two then runs the rungs in ascending
// version order. A single pass that looked each rung up as it reached it would
// run rung 2's statements before discovering rung 3 was missing.
//
// Runs INSIDE a transaction the caller has already opened, and neither begins
// nor commits one: a half-applied upgrade to a store whose only rebuild path
// is the export is the worst outcome available, so the atomicity has to belong
// to one owner and that owner is createSchema(). The stamp is durable only
// when the caller commits.
//
// Degenerate ranges, stated because this is public and test-injectable:
// `from >= to` is an empty climb — nothing validated, nothing run, nothing
// stamped, returns true. `from < 1` is REFUSED: version 0 is a store with no
// schema at all, and creating one is the DDL's job, not a rung's. `from` is
// trusted rather than re-read; the caller has already read `user_version`
// inside its own transaction and a second authoritative read would race it.
//
// Public, and taking its ladder as an argument, for the reason
// kDefaultHistoryCapBytes is a constructor parameter (INV-14): at
// kSchemaVersion 1 the production ladder is EMPTY, so a ladder reachable only
// from production is a ladder nothing can exercise until the first bump.
static bool applyUpgrades(QSqlDatabase &db, int from, int to,
                          const QVector<Upgrade> &ladder, QString *error = nullptr);

// The production ladder — empty at kSchemaVersion 1, because there is no
// version below it to climb from. The first rung lands with the first bump
// (ANTS-3815); INV-4 is what makes that a red test rather than a thing to
// remember.
//
// A public static and not a file-scope object in the .cpp: INV-4's
// completeness check compiles into another translation unit, and a ladder that
// check cannot reach is exactly the failure this section opens by refusing.
// Defined as a function-local static, so it is built once on first call.
static const QVector<Upgrade> &upgradeLadder();
```

Declaration order inside the ladder is irrelevant — both passes key on the
version, not the position.

A rung is SQL and nothing else. A future change needing C++ in a rung (reading
rows out and rewriting them) widens `Upgrade` then, by whoever needs it;
inventing the wider shape now would be scaffolding for a rung that does not
exist.

### 2.2 Where it is called

`createSchema()` gains its fourth arm, between the `version == kSchemaVersion`
return and the DDL:

```cpp
    if (version > 0) {
        if (!applyUpgrades(m_db, version, kSchemaVersion, upgradeLadder(), error)) {
            exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
            return false;
        }
        return exec(m_db, QStringLiteral("COMMIT"), error);
    }
```

Two properties fall out of where it sits rather than needing rules of their
own, which is why neither is an invariant below:

- **Concurrency is already settled.** The version this arm reads is the one
  read inside `BEGIN IMMEDIATE` — ANTS-3756 INV-15's authoritative read. Two
  binaries opening the same behind-version store therefore serialise: the
  winner climbs and commits, and the loser's own read returns
  `kSchemaVersion`, so it takes arm 2 and climbs nothing.
- **`createdSchema()` is untouched.** The flag means "this open made the
  tables"; an upgraded store's tables were made by an earlier binary. INV-5
  pins the flag's single assignment site rather than restating the meaning.

The fast path at the top of `createSchema()` — the unlocked `user_version` read
that returns early on an already-current store — is unchanged and still
correct: a behind-version store misses it and falls through to the
transaction.

### 2.3 Unwelding the two version numbers

`RoadmapExport` gains its own constant in `src/roadmapexport.h`:

```cpp
namespace RoadmapExport {
// The JSONL RECORD shape, which is not the store's table shape. Held apart
// from RoadmapStore::kSchemaVersion (ANTS-3796 § 2.4 hands the split here) so
// a store-side bump does not invalidate every export ever written — an export
// is the store's only rebuild path, and refusing one leaves no recovery route.
// Equal to 1 today because both started there; they move independently now.
//
// `inline` because a namespace-scope `constexpr` in a header has internal
// linkage, giving every TU its own object; `inline constexpr` is the C++17
// idiom for a header constant with one identity.
inline constexpr int kExportSchemaVersion = 1;
}
```

The three live `RoadmapStore::kSchemaVersion` references in
`src/roadmapexport.cpp` — the `meta` record's `schema` value in the writer, and
`rebuildProject()`'s gate and its error message — become
`kExportSchemaVersion`. The fourth mention, a comment in the `section` record
handler reading "the `meta` gate (kSchemaVersion does not move, § 2.4)", is
reworded in the same change: it describes a gate that no longer reads that
constant. **The reword must not name the bare token** — INV-6's grep is
deliberately blind to whether a match is code or comment, and the new spelling
`kExportSchemaVersion` does not contain `kSchemaVersion` as a substring, so a
zero-match assertion is achievable and exact. No *value* changes, so the three
goldens under
`tests/features/roadmap_export_roundtrip/golden/` still carry `"schema":1` and
still import; this change is byte-neutral by construction.

What it buys is what ANTS-3815 needs — but the claim has an edge worth stating,
because the unqualified version is false. A table-shape bump that does not
touch the record shape leaves every existing export **past the `meta` gate**.
It does not make the rebuild total: a rung adding a `NOT NULL` column with no
default leaves `rebuildProject()` inserting rows from a record that carries no
value for it, and that gap belongs to whichever bump introduces such a column
(ANTS-3815) or to ANTS-3860. Unwelding removes a *version* refusal, not the
obligation to give a new column a value the old record shape can supply.

**The store's version stops being recorded in the export, and that is
deliberate.** ANTS-3756 § 2.3 justified the `meta.schema` field as telling "a
future reader … which schema wrote a given file". After the split that field
answers which *record* shape wrote it, which is the question a reader of an
export actually has: the rebuild inserts into whatever table shape the running
binary creates, so the exporting binary's table version constrains nothing on
the way back in. No second field is added — one would be a number nothing
reads, and ANTS-3756's own sentence is corrected in § 7 rather than preserved
by manufacturing a field to keep it true.

## 3. Invariants

- **INV-1** — `applyUpgrades()` applies exactly one rung per version step in
  `(from, to]`, in **ascending version order**, regardless of the order rungs
  appear in the ladder, and stamps `user_version` to `to` after the last rung.
  *Test:* `roadmap_store_upgrade` — with the test holding its own
  `BEGIN IMMEDIATE` (INV-3 forbids the function from opening one), climb 1 → 3
  with the two rungs declared in descending order, where rung 3 references a
  column rung 2 adds; assert both landed and `user_version` is 3. *Breaks
  when:* the ladder is walked in declaration order, which runs rung 3 against a
  column that does not exist yet; or the stamp is left to the caller, which
  passes `createSchema()`'s path and fails every other caller's.
- **INV-2** — A version in `(from, to]` with **no rung, or with more than
  one**, is refused **before any statement runs**: the call returns false, the
  error names that version, and the store is unchanged. *Test:*
  `roadmap_store_upgrade`, three legs — (a) a ladder missing the rung for 2
  while climbing 1 → 2; (b) a ladder holding two rungs both landing on 2; and
  (c) **a 1 → 3 climb with rung 2 present and rung 3 absent**, asserting rung
  2's effect is *not* present. Each asserts false, an error containing the
  offending version, and `user_version` still 1. **Leg (c) is the one that
  earns the invariant:** legs (a) and (b) are single-step, so a lazy per-rung
  lookup that validates as it goes passes both while breaking the "before any
  statement runs" clause — the property this invariant exists for. *Breaks
  when:* validation is folded into the run loop (fails (c)); or the walk skips
  a missing version and stamps the target anyway, producing a store
  **labelled** current whose tables are a version behind — undetectable
  afterwards, because the label is the only evidence of what shape the tables
  are in.
- **INV-3** — `applyUpgrades()` neither begins nor commits a transaction: it
  runs entirely inside the caller's, so a failed rung leaves the caller free to
  roll back and nothing partial survives. *Test:* `roadmap_store_upgrade` —
  inside a `BEGIN IMMEDIATE`, run a two-statement rung whose second statement
  is invalid SQL; assert false, `ROLLBACK`, then assert the first statement's
  effect is absent and `user_version` is still 1. *Breaks when:* it opens its
  own transaction, which SQLite refuses to nest — so the call fails against a
  store that is fine — or commits per rung, which makes a mid-ladder failure
  durable.
- **INV-4** — The production ladder is **complete**: for every version `v` in
  `[1, kSchemaVersion)` there is exactly one rung landing on `v + 1`. *Test:*
  `roadmap_store_upgrade` walks that range against
  `RoadmapStore::upgradeLadder()` — reachable from the test's translation unit
  because § 2.1 declares it in the header, which is that decision's whole
  purpose. **Green and vacuous at `kSchemaVersion` 1, by construction: the
  range is empty. It is a standing guard, not a red-first test** — it fires on
  the first bump, which is the only moment it can. *Breaks when:*
  `kSchemaVersion` is bumped without its rung, which is precisely how a
  version-1 store comes to meet a version-2 build with nothing to climb.
- **INV-5** — An open that upgrades does not report `createdSchema()`:
  `m_createdSchema` is assigned in exactly one place, on `createSchema()`'s
  creation path. *Test:* `roadmap_store_upgrade` greps `src/roadmapstore.cpp`
  for assignments to `m_createdSchema` and asserts exactly one. **Green against
  today's source by construction — a regression fence, not a red-first test**
  (today's source already has exactly one; § 6 says what this costs and what
  replaces the red). Behavioural coverage arrives with the first real rung
  (ANTS-3815): at `kSchemaVersion` 1 no store can be upgraded through `open()`
  at all, which is why the surface is a grep and is named as one. *Breaks
  when:* the new arm sets the flag, telling a caller that this open made tables
  an earlier binary made.
- **INV-6** — The export's record version and the store's table version are
  **separate constants**: `src/roadmapexport.cpp` no longer names the store's
  constant at all, and the three goldens still import unchanged. *Test:*
  `roadmap_store_upgrade` greps `src/roadmapexport.cpp` for the **bare token**
  `kSchemaVersion` — not the qualified spelling — and asserts **zero** matches,
  comments included. The bare token is deliberate and the qualification is not
  enough: the file's fourth mention today is a comment, § 2.3 rewords it in the
  same change, and a grep that skips comments would let a stale one describing
  the welded gate survive. `roadmap_export_roundtrip` unchanged and still green
  is the byte-neutrality leg. *Breaks when:* a later edit reaches for the
  store's constant in the export because both read 1 — re-welding them
  silently, and invisibly until the first bump refuses every export on disk.
- **INV-7** — Every refusal on the upgrade path names **both versions and the
  offending rung**, so § 1's consequence 2 is closed for the failure modes this
  spec introduces rather than only for the one it inherited. A missing or
  duplicated rung names the store's version, the target, and the version whose
  rung was wrong; a rung whose SQL fails names those three plus the SQL error.
  *Test:* `roadmap_store_upgrade` asserts each error string contains the source
  version, the target version and the failing rung's version, across the INV-2
  legs and one leg whose rung holds invalid SQL. *Breaks when:* the failure is
  reported through the file-scope `exec()` helper alone, whose message is the
  SQL error plus the first 120 characters of the statement — naming neither
  version, which is exactly the illegibility § 1 complains of, reproduced one
  layer up.

## 4. RAM / build cost

The ladder is a function-local `static const QVector<Upgrade>` inside
`upgradeLadder()`, empty at version 1, and a rung is a handful of strings —
bytes, not a budget. No eviction policy is owed: the ladder's size is bounded
by `kSchemaVersion`, which grows by one per schema change, and it is
constructed once per process on first call.

**Time is the budget that actually binds here, and it belongs to the rung
author.** The climb runs inside `createSchema()`'s `BEGIN IMMEDIATE`, so it
holds the write lock for its whole duration while every concurrent opener
blocks against `kBusyTimeoutMs` (5000 ms; `kBulkBusyTimeoutMs` is 30000 for the
Bulk profile). A ladder slower than that deadline does not fail *itself* — it
turns an ordinary concurrent open into a `SQLITE_BUSY` failure, which is the
case § 2.2 calls already settled and would stop being so. Today's cost is zero
(no rungs). The constraint this spec fixes is on whoever adds one: **a rung's
statements must complete well inside `kBusyTimeoutMs` on a full-sized store**,
and a migration that cannot is a migration that must move out of `open()`
rather than one that raises the deadline. No new external libraries.

No new build target and no new binary: one test TU joins `test_core`'s
`SOURCES` list (`ants_add_core_bundle`, which already links
`ants_roadmapstore_lib`). No new compile definition is needed either — the two
source-grep invariants locate their files through `ANTS_SRC_DIR`, which
`target_compile_definitions(test_core PRIVATE …)` already defines as
`${CMAKE_SOURCE_DIR}/src` for ANTS-3758's refit scrape.

## 5. Out of scope

- **Bumping `kSchemaVersion`, and the first rung** — ANTS-3815, which is the
  first change that needs one. It also owns retiring the
  `EXPECT_EQ(RoadmapStore::kSchemaVersion, 1)` leg of
  `Inv27SchemaVersionStillOne` in
  `tests/features/roadmap_store_schema/test_roadmap_store_schema.cpp`: that
  assertion is the tripwire ANTS-3782 and ANTS-3796 both leaned on, and it is
  correct until the bump it was built to catch is the intended one. **This spec
  is not the change that comment governs.** Its "LAST change entitled to hold
  at 1" wording is about changes that alter the *record shape* and could have
  been expressed as a bump; this one adds no column and touches no table, so it
  holds at 1 without spending that entitlement.
- **The export-side upgrade path** — ANTS-3860. § 2.3 removes the *coupling*,
  so a table-only bump no longer invalidates exports; it does not give
  `rebuildProject()` a way to import a record shape older than the one it
  knows. That case becomes live once ANTS-3855's verb starts producing real
  exports, and ANTS-3860 records the two shapes it could take.
- **Downgrade — a store whose `user_version` is *higher* than the binary's.**
  Permanent exclusion, not deferred work: ANTS-3756 § 2.3 settled it (refused
  outright, not opened read-only, because a newer schema can move meaning
  rather than only add to it) and this spec does not revisit a decision it is
  completing the other half of.
- **A `PRAGMA integrity_check` or any verification of the upgraded store.**
  Permanent exclusion. An upgrade runs inside one transaction against a store
  SQLite already opened; a checker here would be asserting SQLite's own
  durability, which no rung of this ladder can affect.

## 6. Tests

Feature test: `tests/features/roadmap_store_upgrade/` (`spec.md` +
`test_roadmap_store_upgrade.cpp`), added to `test_core`'s `SOURCES` list in
`CMakeLists.txt` beside the other `roadmap_store_*` directories — **not** as a
new `add_executable`, per `tests/features/README.md`'s bundle rule. Label
`features;fast`. Covers INV-1 … INV-7.

A separate directory rather than an extension of `roadmap_store_schema/`: that
directory's `spec.md` is ANTS-3756's contract, and the ladder is this
document's.

**Verifying RED first — and being honest about which invariants can.** These
invariants mostly cover a function that does not exist yet, so "fails against
pre-fix source" cannot mean a compile against it, and the obvious substitute
(stub `applyUpgrades()` to `return false`, watch everything redden) does not
work: a stub that refuses everything *satisfies* most of these clauses. Against
such a stub:

| Invariant | Against a `return false` stub | Real RED proof |
|---|---|---|
| INV-1 | **RED** — asserts both rungs landed and `user_version` is 3 | the stub |
| INV-2 | green for the wrong reason — a stub refuses every case | mutation: fold validation into the run loop, leg (c) reddens |
| INV-3 | green for the wrong reason — nothing ran, so nothing survives rollback | mutation: wrap the body in its own `BEGIN`/`COMMIT` |
| INV-4 | green, **vacuously** — the range is empty at version 1 | none available until `kSchemaVersion` moves; standing guard |
| INV-5 | green — today's source already has exactly one assignment | mutation: add a second assignment on the upgrade arm |
| INV-6 | **RED** — `kSchemaVersion` appears in `src/roadmapexport.cpp` four times today | today's source |
| INV-7 | green for the wrong reason — a stub's empty error contains nothing to check | mutation: report through `exec()`'s message alone |

**So only INV-1 and INV-6 have a genuine pre-fix RED, and the other five are
proved by mutation against the finished implementation** — each mutation is the
invariant's own *Breaks when:* clause, applied and reverted. That is this
project's established practice for invariants whose subject does not pre-exist
(ANTS-3756's loop log row `8-impl` records the same method, including one named
break that did *not* redden being written down rather than dropped). Record
each mutation's result in the implementation row; a *Breaks when:* clause that
fails to redden is a finding, not a formality.

INV-4 is the one with no proof available at all, and it is left that way
deliberately rather than strengthened into something assertable: any change
that makes it non-vacuous today would have to invent a version to climb, which
is the manufactured-upgrade move ANTS-3782 and ANTS-3796 both refused.

## 7. Cross-doc impact

Two lists, because they are different questions. **Files this change edits**
comes first; **files that merely mention the id** comes second, and the two
overlap only partly — which is why an earlier draft of this section, scoped to
the id, missed three of the four edits below.

**Edited by this change, beyond the two source files § 2.1–2.3 name:**

- `src/roadmapstore.h` — the comment on `kSchemaVersion` reads "Schema version
  carried in PRAGMA user_version; the export's `meta` record carries the same
  number." § 2.3 makes the second clause false. Reworded in the same commit.
- `src/roadmapexport.cpp` — the comment in the `section` record handler reading
  "the `meta` gate (kSchemaVersion does not move, § 2.4)" describes a gate that
  no longer reads that constant. INV-6's bare-token grep is what keeps this
  from being forgotten.
- `tests/features/roadmap_store_schema/test_roadmap_store_schema.cpp` —
  `Inv27SchemaVersionStillOne`'s failure message rots in **two** clauses, not
  one: it says the upgrade path "does not exist" (corrected — it exists; the
  constant is still held at 1 until ANTS-3815), and it says a bump "invalidates
  three export goldens carrying `\"schema\":1`" (corrected — § 2.3 makes that
  false immediately, since a store bump no longer moves the export's number).
  The `EXPECT_EQ` itself is untouched; only the message.
- [`ANTS-3756`](ANTS-3756-roadmap-store-schema.md) § 2.3 — **two** sentences go
  stale, both to be fixed rather than linked around: "the export's `meta`
  record carries the same number" (§ 2.3 above unwelds them) and "no store
  exists outside a test's temp directory" (§ 1 records a version-1 store on
  this machine).

**Mentions of the id** — six files, this spec excluded
(`grep -rln 'ANTS-3781' src/ docs/ tests/`; the count is within that scope, and
`ROADMAP.md` mentions it too):

- [`ANTS-3756`](ANTS-3756-roadmap-store-schema.md) — § 2.3's ownership sentence
  and the Status line's "two known gaps" both gain a link to this spec; the
  gap is closed, not merely reassigned. (Also in the edit list above.)
- [`ANTS-3782`](ANTS-3782-roadmap-section-provenance.md),
  [`ANTS-3796`](ANTS-3796-section-record-completeness.md),
  [`ANTS-3855`](ANTS-3855-roadmap-migrate-verb.md) — each defers "the schema
  upgrade path" to ANTS-3781 with no link; each gains one. ANTS-3796 § 2.4's
  hand-off of the constant split is answered by § 2.3 above.
- `src/roadmapstore.cpp` — the `source_path` and `position` DDL comments say
  "ANTS-3781 owns what follows"; they now name the path that exists.
- `tests/features/roadmap_store_schema/test_roadmap_store_schema.cpp` — see the
  edit list above; both stale clauses of the same message.
- ROADMAP.md — ANTS-3781 flipped; ANTS-3815's "blocked by ANTS-3781" note
  resolved; ANTS-3860 already filed. **ANTS-3781's own bullet carries the same
  `IF NOT EXISTS` misattribution § 1 corrects** ("that choice is recorded in
  ANTS-3756's loop log, row 8-impl, as the discriminator for INV-15"); annotate
  it rather than leave the roadmap asserting what the spec now denies.
- CHANGELOG.md — one entry under **`Fixed`**, not `Added`: the category is
  derived from `Kind:`, and this spec's kind is `fix`.
- CLAUDE.md — no change. No new file, no new lane, no new build target.

## Cold-eyes loop log

| Loop | Date | Lanes | Severities | Dimensions | Resolution |
|---|---|---|---|---|---|
| 1 | 2026-08-07 | 3 (general-purpose, strong model, identical shared brief) | C 1 · H 3 · M 8 · L 11 · I 0 — verified 23, unverified 1 | dim 5×7, dim 2×5, dim 4×5, dim 7×3, dim 15×3, dim 10×2, dim 9×2, dim 6×2 | **All 23 verified findings fixed; the run's centre of gravity was § 6's must-fail-first recipe, which was wrong for four of the (then) six invariants.** (1) CRITICAL, all three lanes: § 6 told the implementer to watch INV-3, INV-4 and INV-5 go RED against a stub, and none of them can — a `return false` stub *satisfies* INV-3's legs, INV-4 walks an empty range at `kSchemaVersion` 1, and INV-5's "exactly one assignment" is already true of today's source. § 6 is now a per-invariant table: only INV-1 and INV-6 have a genuine pre-fix RED, the rest are proved by mutation against the finished implementation (ANTS-3756 row `8-impl`'s own method), and INV-4 is recorded as having no proof available at all rather than being strengthened into one. (2) HIGH, lane 1 alone, and the finding that most changed the document: § 1 called the absent `IF NOT EXISTS` "INV-15's discriminator". It is not — verified against ANTS-3756 row `8-impl` and INV-15's own *Breaks when* clause, the discriminator is the `user_version` read inside `BEGIN IMMEDIATE`, and the absent `IF NOT EXISTS` is what makes a regression of it loud. The ROADMAP bullet carries the same misattribution and is annotated. (3) HIGH ×2: `upgradeLadder()` was specified file-scope in the `.cpp` while INV-4's test calls it from another TU — it is now a public static, which is the same argument § 2.1 already made for `applyUpgrades()`; and "refused before any statement runs" was unobservable because both INV-2 legs were single-step, so a lazy per-rung lookup passed them — § 2.1 now specifies a validate-everything pre-pass and INV-2 gains a multi-step leg (c). (4) MEDIUM: stamping lived only in prose and is now in the declaration comment; degenerate ranges (`from >= to`, `from < 1`) pinned; a time budget added against `kBusyTimeoutMs`, the deadline the climb blocks other openers against; the "every export stays importable" claim qualified (unwelding removes a *version* refusal, not a `NOT NULL` column's missing default); INV-6's grep spelling pinned to the bare token; § 7 rebuilt as two lists after it missed three of the four files this change actually edits. New **INV-7** closes § 1's consequence 2, which no invariant had covered for a failing rung. **Unverified (1):** lane 3's "no other member of this header uses `QVector`" — false, `listItems()`/`listSections()`/`listElements()` all return one, so `QVector<Upgrade>` matches the header and stays; the `QStringList` half of that finding was correct and applied. |
