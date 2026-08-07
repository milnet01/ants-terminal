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

**Sections.** [1 Problem](#1-problem) · [2 Surface](#2-surface) — [2.1 the
ladder](#21-the-ladder), [2.2 where it is called](#22-where-it-is-called),
[2.3 unwelding the two version numbers](#23-unwelding-the-two-version-numbers)
· [3 Invariants](#3-invariants) · [4 RAM / build cost](#4-ram--build-cost) ·
[5 Out of scope](#5-out-of-scope) · [6 Tests](#6-tests) ·
[7 Cross-doc impact](#7-cross-doc-impact)

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
`IF NOT EXISTS`: INV-15's own *Breaks when* clause names gating on
`IF NOT EXISTS` as the **failure** ("succeeds for both and reports nothing"),
not the mechanism. The absent `IF NOT EXISTS` is what makes a regression of
that mechanism **loud**. (ANTS-3756's loop log row `8-impl` records the same
reasoning from the implementation side; INV-15 is the contract and is cited
here on its own.) The effect on an older store is the same either way, and it
is the wrong one: it is not re-created, it dies on `table already exists`.
Three consequences:

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
`kSchemaVersion` is 1, so no store *strictly between 0 and* `kSchemaVersion`
can exist (a store at 0 exists and is the creation case).

**Two different milestones are involved and ANTS-3756 § 2.3 conflates them**,
which is why this spec names a different one than that document does. A store
becomes **reachable** at ANTS-3758's cutover — real stores in real hands, which
is the deadline ANTS-3756 records. A store *below* the running binary's version
becomes **possible** only once `kSchemaVersion` moves, which is ANTS-3815.
Reachability is what makes the gap matter; the bump is what makes it
occur. § 7 lists that sentence for correction rather than working around it. A
version-1 store file exists on this machine now, so the first half has already
happened.

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
// PRECONDITION: the caller has an open transaction. This function neither
// begins nor commits one — a half-applied upgrade to a store whose only
// rebuild path is the export is the worst outcome available, so the atomicity
// belongs to one owner and that owner is createSchema(). The stamp is durable
// only when the caller commits. There is no runtime guard for the
// precondition, and § 5 records why: QSqlDatabase does not surface
// sqlite3_get_autocommit() (the same limitation m_inTransaction exists for),
// so the check cannot be made without inventing a second one.
//
// Degenerate arguments, IN THIS ORDER, because two of the rules overlap and
// the order is what makes the result single-valued:
//   1. `from < 1` REFUSES, whatever `to` is. Version 0 is a store with no
//      schema at all and creating one is the DDL's job, not a rung's; a
//      negative version is a corrupt pragma. This arm is why createSchema()
//      can route every non-zero version here and still fail legibly (§ 2.2).
//   2. otherwise `from >= to` is an empty climb — nothing validated, nothing
//      run, nothing stamped, returns true. Note this makes a DOWNGRADE range
//      a silent no-op rather than a refusal; refusing a downgrade is
//      createSchema()'s `version > kSchemaVersion` arm and stays there (§ 5).
//   3. otherwise the two passes above run over `(from, to]`.
// `to` is NOT compared to kSchemaVersion — a test climbs past it by design
// (INV-1), and this function's job is the climb, not the policy.
// Rungs whose `to` falls outside `(from, to]` are ignored; a rung with an
// empty `statements` list is legal and advances the version by itself.
//
// `from` is trusted rather than re-read. Not because a re-read would race —
// inside the caller's write lock it could not — but because the version is an
// argument for the same reason the ladder is: a function that read its own
// starting point could only ever be tested against a store already at it.
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
// check cannot reach leaves the missing-rung case — the one INV-2 refuses at
// runtime — with nothing checking for it at build time, which is the whole
// point of having INV-4.
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
    // Everything that is not 0 comes here, negatives included: a corrupt
    // pragma must not reach the DDL, which would answer it with `table
    // already exists` — the illegible failure § 1 exists to close. The
    // `from < 1` arm above refuses it naming the value instead.
    if (version != 0) {
        if (!applyUpgrades(m_db, version, kSchemaVersion, upgradeLadder(), error)) {
            exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
            return false;
        }
        return exec(m_db, QStringLiteral("COMMIT"), error);
    }
```

The guard is `!= 0` rather than `> 0` deliberately. `user_version` is a signed
32-bit pragma, so a negative value is representable; under `> 0` it would fall
through to the DDL and reproduce the exact failure this spec is closing, one
input away from the case everyone thinks about.

Two properties fall out of where the arm sits rather than needing rules of
their own:

- **Concurrency is already settled.** The version this arm reads is the one
  read inside `BEGIN IMMEDIATE` — ANTS-3756 INV-15's authoritative read. Two
  binaries opening the same behind-version store therefore serialise: the
  winner climbs and commits, and the loser's own read returns
  `kSchemaVersion`, so it takes arm 2 and climbs nothing.
- **`createdSchema()` is untouched.** The flag means "this open made the
  tables"; an upgraded store's tables were made by an earlier binary. This one
  *does* have an invariant — INV-5 pins the flag's single assignment site
  rather than restating the meaning; only the concurrency property above is
  left to ANTS-3756 INV-15.

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
  statement runs" clause — the property this invariant exists for. **What the
  error must *say* is INV-7's, not this one's** — this invariant asserts only
  that the offending version appears, so that an implementation satisfying it
  cannot fail INV-7's stronger contract. *Breaks
  when:* validation is folded into the run loop (fails (c)); or the walk skips
  a missing version and stamps the target anyway, producing a store
  **labelled** current whose tables are a version behind — undetectable
  afterwards, because the label is the only evidence of what shape the tables
  are in.
- **INV-3** — `applyUpgrades()` neither begins nor commits a transaction: it
  runs entirely inside the caller's, so a failed rung leaves the caller free to
  roll back and nothing partial survives. *Test:* `roadmap_store_upgrade`, two
  legs. (a) Behavioural — inside a `BEGIN IMMEDIATE`, run a two-statement rung
  whose second statement is invalid SQL; assert false, `ROLLBACK`, then assert
  the first statement's effect is absent and `user_version` is still 1.
  (b) Source-grep — `applyUpgrades()`'s body in `src/roadmapstore.cpp` contains
  no `BEGIN`, `COMMIT`, `ROLLBACK` or `SAVEPOINT`. Leg (b) is not redundant:
  leg (a) runs with a transaction already open, which is precisely the
  condition under which a stray `BEGIN` fails harmlessly and invisibly, so the
  behavioural leg cannot see the defect that matters when the precondition is
  broken. *Breaks when:* it opens its own transaction, which SQLite refuses to
  nest — so the call fails against a store that is fine — or commits per rung,
  which makes a mid-ladder failure durable.
- **INV-4** — The production ladder is **complete**: for every version `v` in
  `[1, kSchemaVersion)` there is exactly one rung landing on `v + 1`, **and no
  rung lands outside that range** — a stray rung is a rung nothing will ever
  climb, which is an authoring error the same table can catch for free.
  *Test:* `roadmap_store_upgrade` walks that range against
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
  for assignments to `m_createdSchema` and asserts exactly one **and that it
  falls on the creation path** — after the `PRAGMA user_version = %1` stamp,
  inside `createSchema()`. **Both legs, because a count alone is green against
  the very regression this invariant names:** *moving* the single assignment
  onto the upgrade arm leaves the count at one. **Green against
  today's source by construction — a regression fence, not a red-first test**
  (today's source already has exactly one; § 6 says what this costs and what
  replaces the red). Behavioural coverage arrives with the first real rung
  (ANTS-3815): at `kSchemaVersion` 1 no store can be upgraded through `open()`
  at all, which is why the surface is a grep and is named as one. *Breaks
  when:* the new arm sets the flag, telling a caller that this open made tables
  an earlier binary made.
- **INV-6** — The export's record version and the store's table version are
  **separate constants**: neither export translation unit names the store's
  constant at all, and the three goldens still import unchanged. *Test:*
  `roadmap_store_upgrade` greps **both `src/roadmapexport.cpp` and
  `src/roadmapexport.h`** for the substring `kSchemaVersion` — which also
  matches the qualified `RoadmapStore::kSchemaVersion`, and does **not** match
  the new `kExportSchemaVersion`, whose preceding character is `t` — and
  asserts **zero** matches, comments included. Three choices, each load-bearing:
  the header is in scope because § 2.3 puts the new constant there and a
  re-weld through it would otherwise be invisible; comments are in scope
  because the file's fourth mention today *is* a comment that § 2.3 rewords;
  and the scope stops at these two files rather than `src/` + `tests/`, because
  `src/roadmapstore.{h,cpp}` and the tripwire test legitimately keep naming the
  store's own constant. `roadmap_export_roundtrip` unchanged and still green
  is the byte-neutrality leg. *Breaks when:* a later edit reaches for the
  store's constant in the export because both read 1 — re-welding them
  silently, and invisibly until the first bump refuses every export on disk.
- **INV-7** — Every refusal on the upgrade path names **the store's version and
  the target version**, so § 1's consequence 2 is closed for the failure modes
  this spec introduces rather than only for the one it inherited. Four
  refusals, enumerated because an unenumerated one is the one that ships with
  `exec()`'s message: a **missing or duplicated rung** adds the version whose
  rung was wrong; a **rung whose SQL fails** adds that rung's version and the
  SQL error; a **failed `user_version` stamp** adds the SQL error; and a
  **`from < 1` refusal** adds the offending value and says version 0 is the
  DDL's case — it has no rung to name, which is why "the offending rung" is not
  part of the invariant's common clause.
  *Test:* `roadmap_store_upgrade` asserts each error string contains the source
  and target versions, across the three INV-2 legs, one leg whose rung holds
  invalid SQL, and one `from = 0` leg. *Breaks when:* the failure is
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
rather than one that raises the deadline. **The figure is 1000 ms**, a fifth of
`kBusyTimeoutMs`, measured on this project's own roadmap as the largest store
available (~1,839 items); "well inside" without a number is not a budget.
**Enforced by review at ANTS-3815, not by a test** — there is no rung to time
today, and a timing assertion with no subject is a flaky test waiting for one.
No new external libraries.

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
  does not spend that entitlement, on the cited grounds rather than a
  reinterpretation of them.** ANTS-3796 INV-6 grants a *change that would
  otherwise have bumped* `kSchemaVersion` the right not to; this change never
  reaches that question, because it alters no table and so has nothing a bump
  could express.
- **The export-side upgrade path** — ANTS-3860, whose scope and the limits of
  § 2.3's unwelding are stated there and in § 2.3; not restated here. It
  becomes live when a real export first exists on disk — ANTS-3855's verb has
  shipped, so that is a matter of someone running it, not of further work.
- **Downgrade — a store whose `user_version` is *higher* than the binary's.**
  Permanent exclusion, not deferred work: ANTS-3756 § 2.3 settled it (refused
  outright, not opened read-only, because a newer schema can move meaning
  rather than only add to it) and this spec does not revisit a decision it is
  completing the other half of. **The refusal stays in `createSchema()`'s
  `version > kSchemaVersion` arm, and `applyUpgrades()` must not grow one:**
  its degenerate rule 2 makes a `from > to` range a silent empty climb, which
  reads like a gap and is not one — by the time the arm below runs, the
  downgrade case has already been refused two branches above. Adding a second
  refusal inside `applyUpgrades()` would break the empty-climb contract INV-1's
  own test relies on.
- **A runtime guard for `applyUpgrades()`'s open-transaction precondition.**
  Permanent exclusion. SQLite exposes autocommit state through
  `sqlite3_get_autocommit()`, which `QSqlDatabase` does not surface — the same
  limitation `src/roadmapstore.h` records as the reason `m_inTransaction`
  exists as a member at all — and the store's own flag is not visible to a
  static function taking a bare `QSqlDatabase`. So the precondition is a caller
  obligation, held by the one production caller two lines above the call, and
  INV-3's leg (b) is what keeps the function from quietly acquiring a
  transaction of its own instead.
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
| INV-5 | green — today's source already has exactly one assignment | mutation: **move** the assignment onto the upgrade arm (not *add* a second — a second is caught by the count leg, and moving it is the regression the invariant actually names) |
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

**One table, and it is the complete edit set** — every file this change touches
for any reason, with what changes and why. It replaces the two overlapping
lists an earlier draft carried (one keyed on "files this edits", one on "files
mentioning ANTS-3781"): those partly overlapped, so each had to restate the
other's rows, and both went out of date on the first fix. A row's absence here
means the file is untouched.

| File | Change | Why |
|---|---|---|
| `src/roadmapstore.h` | Add `Upgrade`, `applyUpgrades()`, `upgradeLadder()` (§ 2.1). Reword the `kSchemaVersion` comment: "the export's `meta` record carries the same number" is false after § 2.3. | The three new symbols, plus a comment asserting the welding § 2.3 removes. |
| `src/roadmapstore.cpp` | Implement the two passes and the ladder; add `createSchema()`'s `version != 0` arm (§ 2.2). Update the `source_path` / `position` DDL comments, which say "ANTS-3781 owns what follows". | The change itself, and two comments naming this item as unbuilt. |
| `src/roadmapexport.h` | Add `inline constexpr int kExportSchemaVersion = 1` (§ 2.3). | The unweld's other half. |
| `src/roadmapexport.cpp` | Three live references → `kExportSchemaVersion`; reword the `section`-handler comment "the `meta` gate (kSchemaVersion does not move, § 2.4)" **without using the token**. | INV-6 asserts zero matches across both export TUs, comments included. |
| `CMakeLists.txt` | One line: the new test TU into `test_core`'s `SOURCES`. | No new target, no new define (§ 4). |
| `tests/features/roadmap_store_upgrade/` | New — `spec.md` + `test_roadmap_store_upgrade.cpp`. | § 6. |
| `tests/features/roadmap_store_schema/test_roadmap_store_schema.cpp` | `Inv27SchemaVersionStillOne`'s failure message rots in **three** clauses: the upgrade path "does not exist" (it now does); a bump "invalidates three export goldens" (§ 2.3 makes that false immediately); and "against ZERO stores that would need one" (§ 1 records a version-1 store on this machine, which a bump *would* strand). The `EXPECT_EQ` itself is untouched. | The message is this item's own obituary and all three clauses outlive it. |
| [`ANTS-3756`](ANTS-3756-roadmap-store-schema.md) | § 2.3: **three** stale sentences corrected — "the export's `meta` record carries the same number" (unwelded), "no store exists outside a test's temp directory" (one does), and "It stops being unreachable at ANTS-3758's cutover" (§ 1 separates reachable from below-version). Ownership sentence and the Status line's "two known gaps" gain a link here. | This spec completes that document's version contract; leaving it contradicted is the most expensive residue available. |
| [`ANTS-3796`](ANTS-3796-section-record-completeness.md) | § 2.4: "`rebuildProject()` aborts on `schema != RoadmapStore::kSchemaVersion`" and "the two are one constant, and separating them is ANTS-3781's problem" — both false once § 2.3 lands; the hand-off is answered rather than pending. INV-6's *Breaks when* likewise. | A correction, not a link. It is the document that assigned § 2.3's work here. |
| [`ANTS-3782`](ANTS-3782-roadmap-section-provenance.md) | "ANTS-3781 owns the upgrade path that is **still missing**" — no longer missing. | Same class. |
| [`ANTS-3855`](ANTS-3855-roadmap-migrate-verb.md) | Defers "the schema upgrade path" to ANTS-3781; gains a link. | Link only — its sentence stays true. |
| `ROADMAP.md` | ANTS-3781 flipped; ANTS-3815's "blocked by ANTS-3781" note resolved; ANTS-3860 already filed. **ANTS-3781's own bullet repeats the `IF NOT EXISTS` misattribution § 1 corrects** and is annotated. | The roadmap otherwise asserts what the spec now denies. |
| `CHANGELOG.md` | One entry under **`Fixed`**. | Category derives from `Kind:`, which is `fix` — not `Added`. |
| `CLAUDE.md` | No change. | No new file, no new lane, no new build target. |

## Cold-eyes loop log

| Loop | Date | Lanes | Severities | Dimensions | Resolution |
|---|---|---|---|---|---|
| 2 | 2026-08-07 | 3, cold — same shared brief, no mention of loop 1 | C 0 · H 4 · M 9 · L 12 · I 0 — verified 25, unverified 1 | dim 5×6, dim 7×5, dim 10×4, dim 4×4, dim 2×3, dim 15×3, dim 1×2, dim 6×2, dim 9×2, dim 11×2 | **No CRITICAL, and roughly half the batch was collateral from loop 1's own edits — so the answer was to consolidate rather than patch again.** § 7, rewritten in loop 1 as two lists (edits / id-mentions), drew three lanes' HIGHs: the lists partly overlapped so each restated the other, the preamble miscounted its own scope, and it still missed ANTS-3796 § 2.4 — the very document that assigned § 2.3's work here, whose prose § 2.3 makes false. Replaced with **one table that is the complete edit set**, which removes the permanent finding source rather than correcting this instance of it. Also collateral: loop 1's degenerate-range rules (`from < 1` and `from >= to`) overlapped with opposite outcomes and no precedence — now ordered and numbered, with the downgrade case explicitly left to `createSchema()`'s existing arm so nobody "fixes" the empty climb INV-1 depends on; and loop 1's INV-7 was unsatisfiable for the one refusal that has no rung to name, now enumerated over all four refusals. **Draft defects, present since the first draft:** `createSchema()`'s new arm guarded `version > 0`, so a negative `user_version` — representable, the pragma is signed — still fell through to the DDL and reproduced the illegible failure this spec exists to close (now `!= 0`, routed to a legible refusal); INV-5 asserted the flag's *location* but tested only its *count*, so **moving** the single assignment onto the upgrade arm — the exact regression its own *Breaks when* names — left it green (location leg added, § 6's mutation corrected from "add a second" to "move the existing one"); § 1 named ANTS-3815 as the deadline where ANTS-3756 § 2.3 names ANTS-3758's cutover, which are two different events (a store becomes *reachable* at the cutover, *below-version* only at a bump) — both now stated and ANTS-3756's sentence added to the edit table. § 4's "well inside `kBusyTimeoutMs`" gained a figure (1000 ms) and an honest enforcement note (review at ANTS-3815, not a test). A 525-line spec gained a section index. **Unverified (1):** a lane read the Layman line's "refuses to open" as contradicting § 1 — it does not; `createSchema()` returns false and `open()` propagates it, so a refusal is exactly what happens. **Found while verifying, filed not fixed:** ANTS-3861, a doubled `src/src/` path in an unrelated test's `ANTS_SRC_DIR` fallback. |
| 1 | 2026-08-07 | 3 (general-purpose, strong model, identical shared brief) | C 1 · H 3 · M 8 · L 11 · I 0 — verified 23, unverified 1 | dim 5×7, dim 2×5, dim 4×5, dim 7×3, dim 15×3, dim 10×2, dim 9×2, dim 6×2 | **All 23 verified findings fixed; the run's centre of gravity was § 6's must-fail-first recipe, which was wrong for four of the (then) six invariants.** (1) CRITICAL, all three lanes: § 6 told the implementer to watch INV-3, INV-4 and INV-5 go RED against a stub, and none of them can — a `return false` stub *satisfies* INV-3's legs, INV-4 walks an empty range at `kSchemaVersion` 1, and INV-5's "exactly one assignment" is already true of today's source. § 6 is now a per-invariant table: only INV-1 and INV-6 have a genuine pre-fix RED, the rest are proved by mutation against the finished implementation (ANTS-3756 row `8-impl`'s own method), and INV-4 is recorded as having no proof available at all rather than being strengthened into one. (2) HIGH, lane 1 alone, and the finding that most changed the document: § 1 called the absent `IF NOT EXISTS` "INV-15's discriminator". It is not — verified against ANTS-3756 row `8-impl` and INV-15's own *Breaks when* clause, the discriminator is the `user_version` read inside `BEGIN IMMEDIATE`, and the absent `IF NOT EXISTS` is what makes a regression of it loud. The ROADMAP bullet carries the same misattribution and is annotated. (3) HIGH ×2: `upgradeLadder()` was specified file-scope in the `.cpp` while INV-4's test calls it from another TU — it is now a public static, which is the same argument § 2.1 already made for `applyUpgrades()`; and "refused before any statement runs" was unobservable because both INV-2 legs were single-step, so a lazy per-rung lookup passed them — § 2.1 now specifies a validate-everything pre-pass and INV-2 gains a multi-step leg (c). (4) MEDIUM: stamping lived only in prose and is now in the declaration comment; degenerate ranges (`from >= to`, `from < 1`) pinned; a time budget added against `kBusyTimeoutMs`, the deadline the climb blocks other openers against; the "every export stays importable" claim qualified (unwelding removes a *version* refusal, not a `NOT NULL` column's missing default); INV-6's grep spelling pinned to the bare token; § 7 rebuilt as two lists after it missed three of the four files this change actually edits. New **INV-7** closes § 1's consequence 2, which no invariant had covered for a failing rung. **Unverified (1):** lane 3's "no other member of this header uses `QVector`" — false, `listItems()`/`listSections()`/`listElements()` all return one, so `QVector<Upgrade>` matches the header and stays; the `QStringList` half of that finding was correct and applied. |
