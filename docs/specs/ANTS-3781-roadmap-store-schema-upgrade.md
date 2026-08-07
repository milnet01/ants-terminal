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
meets a newer build, nothing knows how to bring it up to date — it just
crashes. This adds the missing "bring it up to date" step, and stops a database
change from spoiling the backup files at the same time.

## 1. Problem

`RoadmapStore::createSchema()` (`src/roadmapstore.cpp`) branches on the version
it reads inside its `BEGIN IMMEDIATE`, and has three arms where it needs four:

1. `version > kSchemaVersion` — refused, naming both numbers.
2. `version == kSchemaVersion` — commits and returns.
3. `version == 0` — runs the DDL and stamps `user_version`.

A version strictly between 0 and `kSchemaVersion` reaches arm 3. The DDL is
written **without** `IF NOT EXISTS` deliberately — that is
[ANTS-3756](ANTS-3756-roadmap-store-schema.md) INV-15's discriminator, the
thing that makes exactly one of two racing openers the creator — so an older
store does not get re-created, it dies on `table already exists`. Three
consequences, and only the first is about a crash:

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

A rung is a version step and the statements that reach it. Both new symbols are
public on `RoadmapStore`, in `src/roadmapstore.h`:

```cpp
// One rung of the upgrade ladder: the statements that take a store from
// version `to - 1` to version `to`, in the order given.
struct Upgrade {
    int to;
    QVector<QString> statements;
};

// Climbs `from` to `to`, applying exactly one rung per version step.
//
// Runs INSIDE a transaction the caller has already opened, and neither begins
// nor commits one: a half-applied upgrade to a store whose only rebuild path
// is the export is the worst outcome available, so the atomicity has to belong
// to one owner and that owner is createSchema().
//
// Public, and taking its ladder as an argument, for the reason
// kDefaultHistoryCapBytes is a constructor parameter (INV-14): at
// kSchemaVersion 1 the production ladder is EMPTY, so a ladder reachable only
// from production is a ladder nothing can exercise until the first bump.
static bool applyUpgrades(QSqlDatabase &db, int from, int to,
                          const QVector<Upgrade> &ladder, QString *error);
```

The production ladder is file-scope in `src/roadmapstore.cpp` beside the DDL:

```cpp
// Empty at kSchemaVersion 1: there is no version below it, so there is nothing
// to climb. The first rung lands with the first bump (ANTS-3815), and INV-4 is
// what makes that a red test rather than a thing to remember.
const QVector<RoadmapStore::Upgrade> &upgradeLadder();
```

`applyUpgrades()` walks `v` from `from + 1` to `to` inclusive and, for each,
looks up the single rung landing on `v`. **Zero or more than one refuses**,
before any statement runs, with the version in the message. Declaration order
in the ladder is irrelevant — the walk is by version, not by position — and
`user_version` is stamped once, after the last rung.

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
constexpr int kExportSchemaVersion = 1;
}
```

The three live `RoadmapStore::kSchemaVersion` references in
`src/roadmapexport.cpp` — the `meta` record's `schema` value in the writer, and
`rebuildProject()`'s gate and its error message — become
`kExportSchemaVersion`. No value changes, so the three goldens under
`tests/features/roadmap_export_roundtrip/golden/` still carry `"schema":1` and
still import; this change is byte-neutral by construction.

What it buys is what ANTS-3815 needs: a table-shape bump that does not touch
the record shape leaves every existing export importable.

## 3. Invariants

- **INV-1** — `applyUpgrades()` applies exactly one rung per version step in
  `(from, to]`, in **ascending version order**, regardless of the order rungs
  appear in the ladder. *Test:* `roadmap_store_upgrade` — climb 1 → 3 with the
  two rungs declared in descending order, where rung 3 references a column rung
  2 adds; assert both landed and `user_version` is 3. *Breaks when:* the ladder
  is walked in declaration order, which runs rung 3 against a column that does
  not exist yet.
- **INV-2** — A version in `(from, to]` with **no rung, or with more than
  one**, is refused before any statement runs: the call returns false, the
  error names that version, and the store is unchanged. *Test:*
  `roadmap_store_upgrade`, two legs — a ladder missing the rung for 2 while
  climbing 1 → 2, and a ladder holding two rungs both landing on 2; assert
  false, an error containing the version, and `user_version` still 1 with
  neither rung's effect present. *Breaks when:* the walk skips a missing
  version and stamps the target anyway, producing a store **labelled** current
  whose tables are a version behind — undetectable afterwards, because the
  label is the only evidence of what shape the tables are in.
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
  `roadmap_store_upgrade` walks that range against `upgradeLadder()`. **Vacuous
  at `kSchemaVersion` 1, by construction — the range is empty — and that is the
  point:** this is the guard that turns "remember to add a rung when you bump"
  into a red test the moment the constant moves, which is the only moment it
  can fire. *Breaks when:* `kSchemaVersion` is bumped without its rung, which
  is precisely how a version-1 store comes to meet a version-2 build with
  nothing to climb.
- **INV-5** — An open that upgrades does not report `createdSchema()`:
  `m_createdSchema` is assigned in exactly one place, the `version == 0`
  creation branch. *Test:* `roadmap_store_upgrade` source-greps
  `src/roadmapstore.cpp` for assignments to `m_createdSchema` and asserts
  exactly one, inside `createSchema()`. Behavioural coverage arrives with the
  first real rung (ANTS-3815); at `kSchemaVersion` 1 no store can be upgraded
  through `open()` at all, which is why the surface is a grep and is named as
  one. *Breaks when:* the new arm sets the flag, telling a caller that this
  open made tables an earlier binary made.
- **INV-6** — The export's record version and the store's table version are
  **separate constants**: no reference to `RoadmapStore::kSchemaVersion`
  survives in `src/roadmapexport.cpp`, and the three goldens still import
  unchanged. *Test:* `roadmap_store_upgrade` greps `src/roadmapexport.cpp` for
  `RoadmapStore::kSchemaVersion` and asserts zero live references;
  `roadmap_export_roundtrip` is unchanged and still green, which is the
  byte-neutrality leg. *Breaks when:* a later edit reaches for the store's
  constant in the export because both read 1 — re-welding them silently, and
  invisibly until the first bump refuses every export on disk.

## 4. RAM / build cost

The ladder is a `static const QVector<Upgrade>`, empty at version 1, and a rung
is a handful of `QString`s — bytes, not a budget. No eviction policy is owed:
the ladder's size is bounded by `kSchemaVersion`, which grows by one per
schema change, and it is constructed once per process on first call.

No new build target and no new binary: one test TU joins `test_core`'s
`SOURCES` list (`ants_add_core_bundle`, which already links
`ants_roadmapstore_lib`). No new external libraries.

## 5. Out of scope

- **Bumping `kSchemaVersion`, and the first rung** — ANTS-3815, which is the
  first change that needs one. It also owns retiring the
  `EXPECT_EQ(RoadmapStore::kSchemaVersion, 1)` leg of
  `Inv27SchemaVersionStillOne` in
  `tests/features/roadmap_store_schema/test_roadmap_store_schema.cpp`: that
  assertion is the tripwire ANTS-3782 and ANTS-3796 both leaned on, and it is
  correct until the bump it was built to catch is the intended one.
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
`features;fast`. Covers INV-1 … INV-6.

A separate directory rather than an extension of `roadmap_store_schema/`: that
directory's `spec.md` is ANTS-3756's contract, and the ladder is this
document's.

**Verifying RED first.** These invariants cover a function that does not exist
yet, so "fails against pre-fix source" cannot mean a compile against it. The
equivalent, and what to do: land `applyUpgrades()` as a stub returning `false`,
confirm INV-1, INV-3 and INV-4 go red and INV-2 goes **green for the wrong
reason** — a stub refuses everything, including the cases it should refuse —
then implement. INV-2's green-against-a-stub is the reason its legs assert the
store's *state* after the refusal and not merely the return value.

INV-5 and INV-6 are source greps against `src/roadmapstore.cpp` and
`src/roadmapexport.cpp`; both go red against today's source, INV-6 because
`RoadmapStore::kSchemaVersion` has three live references there now.

## 7. Cross-doc impact

Six files mention ANTS-3781 today, this spec excluded
(`grep -rln 'ANTS-3781' src/ docs/ tests/` → one source file, four specs, one
test):

- [`ANTS-3756`](ANTS-3756-roadmap-store-schema.md) — § 2.3's ownership sentence
  and the Status line's "two known gaps" both gain a link to this spec; the
  gap is closed, not merely reassigned.
- [`ANTS-3782`](ANTS-3782-roadmap-section-provenance.md),
  [`ANTS-3796`](ANTS-3796-section-record-completeness.md),
  [`ANTS-3855`](ANTS-3855-roadmap-migrate-verb.md) — each defers "the schema
  upgrade path" to ANTS-3781 with no link; each gains one. ANTS-3796 § 2.4's
  hand-off of the constant split is answered by § 2.3 above.
- `src/roadmapstore.cpp` — the `source_path` and `position` DDL comments say
  "ANTS-3781 owns what follows"; they now name the path that exists.
- `tests/features/roadmap_store_schema/test_roadmap_store_schema.cpp` —
  `Inv27SchemaVersionStillOne`'s failure message says the upgrade path "does
  not exist". Corrected to say it exists and the constant is still held at 1
  until ANTS-3815.
- ROADMAP.md — ANTS-3781 flipped; ANTS-3815's "blocked by ANTS-3781" note
  resolved; ANTS-3860 already filed.
- CHANGELOG.md — one `Added` entry.
- CLAUDE.md — no change. No new file, no new lane, no new build target.

## Cold-eyes loop log

| Loop | Findings | Resolution |
|---|---|---|
