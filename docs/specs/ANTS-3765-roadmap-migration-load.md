# ANTS-3765 — Roadmap migration, load half: atomicity, re-run matching and the cutover interim

**Status:** spec draft (2026-07-31).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3765 (split out of ANTS-3757 at the read/load seam, 2026-07-31).
**Blocked by:** ANTS-3767 — **cleared 2026-07-31**; `ItemWrite` now carries `lanes`/`evidence`/`extras` and ANTS-3756 INV-21 locks them.
**Pairs with:** [ANTS-3757](ANTS-3757-roadmap-migration-read.md) (produces the plan this consumes), [ANTS-3756](ANTS-3756-roadmap-store-schema.md) (the store this writes to), [ANTS-3761](ANTS-3761-roadmap-export-format.md) (the export that must round-trip what this writes).
**Blocker for:** ANTS-3758.

## 1. Problem

[ANTS-3757](ANTS-3757-roadmap-migration-read.md) ships a pure `RoadmapMigrate::planFrom()` that turns a project's `ROADMAP.md` into an in-memory `MigrationPlan` — sections, items, elements, legend and notes — and deliberately writes nothing. Nothing consumes that plan. Migration is therefore half-built: the corpus can be *read* into a plan and cannot be *loaded* into the store, so **ANTS-3758**'s read verbs — which have no spec yet — have no data to serve and the store shipped by ANTS-3756 stays empty.

Loading is not "call `putItem()` in a loop", and § 1.1 is why.

### 1.1 What the shipped store surface actually supports

Verified against source on 2026-07-31, not recalled. Four findings, and the first one decides this document's shape.

1. **`putItem()` opens and commits its own transaction.** `RoadmapStore::putItem()` begins with `BEGIN IMMEDIATE` and ends with `COMMIT` (`src/roadmapstore.cpp::putItem()`). SQLite does not nest transactions — measured on the version this project builds against:

   ```console
   $ sqlite3 --version
   3.53.2 2026-06-03 …
   $ sqlite3 nest.db "CREATE TABLE t(a); BEGIN IMMEDIATE; BEGIN IMMEDIATE;"
   Error in 2nd command line argument: cannot start a transaction within a transaction
   ```

   So **both halves of the shape the ROADMAP bullet names are unreachable through the shipped API**, in opposite directions. Wrap N `putItem()` calls in a load-half transaction and the first one's `BEGIN` fails, `putItem()` returns `std::nullopt`, and the migration writes zero items. Drop the wrapper and every item self-commits, so a failure at item 400 of 600 leaves 399 committed and no way to complete or undo the rest — per-project atomicity, which is this id's first named concern, is not merely hard but unexpressible. § 2.3 resolves it.

2. **Four things the plan carries have no writer at all.** `PlannedSection::intro`, `PlannedElement` (`kind` `narration` or `table`), `PlannedLegend`, and the `id_prefix` high-water mark § 2.8 must advance. The store's public surface is `registerProject`, `addSection`, `putItem`, `setItemField`, `relateItems`, `relateCrossProject`, `appendHistory`, `historyBytes`, `canonicalJson` (`src/roadmapstore.h`) — `addSection()` takes no `intro` argument, and there is no element, legend or id-prefix writer among them. ANTS-3761's rebuild reaches these columns only by writing raw SQL against `RoadmapStore::db()`, which is already a second producer for the `item` row; a third would be worse. § 2.4 adds the writers instead.

3. **There is no read path for re-run matching.** Nothing on the store answers "does this project already have an item with this id?", which every re-run decision in § 2.6 needs.

4. **The store cannot say which projects are migrated.** No column records it, and the cutover interim (§ 2.10) is the state in which some are and some are not. § 2.10 resolves this without a schema change, because `PRAGMA user_version` is `1` and a migration of the store itself is out of scope for a store that has never shipped data.

Findings 2–4 are additions this id owns rather than defects in ANTS-3756: the store grew the API its callers needed, and this is the first caller to need these. Finding 1 is different in kind — it is a shipped decision that has to change — and § 2.3 argues it on its own terms.

## 2. Surface

### 2.1 The declarations, and the single statement of this half's shape

**The declarations are the contract.** Every later section states a *decision* and refers to these types rather than restating them; § 3's invariants do the same. Where the two could disagree, the declaration wins. This mirrors ANTS-3757 § 2.1, whose consolidation row records why: three copies of one contract produce loops that find disagreements between the copies rather than defects in the design.

New files `src/roadmapmigrateload.{h,cpp}` join **`ants_roadmapstore_lib`**, not `ants_core_lib`. The read half is in `ants_core_lib` precisely because it needs no database; this half needs `Qt6::Sql` and the store, and `ants_core_lib` cannot see `ants_roadmapstore_lib`. The two halves stay in different libraries, which is the seam made mechanical.

```cpp
namespace RoadmapMigrateLoad {

// What one plan's load did. A value, not a log: § 2.11 requires every
// outcome to be assertable by a test rather than read out of stderr.
struct Outcome {
    bool    ok = false;          // false ⇒ NOTHING for this project was committed
    QString error;               // set iff !ok; the first failure, not a list
    qint64  projectId = 0;

    int     itemsInserted = 0;
    int     itemsUpdated = 0;    // matched an existing row, at least one field changed
    int     itemsUnchanged = 0;  // matched, nothing to write (§ 2.6)
    int     itemsOrphaned = 0;   // in the store, absent from source (§ 2.7)
    int     idsAllocated = 0;    // § 2.8
    int     sectionsWritten = 0, elementsWritten = 0, historyRows = 0;

    // Every note the plan carried, plus the ones only the load can raise
    // (§ 2.11's codes). Never a superset of the plan's notes: carried
    // through so one report covers the whole migration of one project.
    QVector<RoadmapMigrate::Note> notes;
};

// The clock is a PARAMETER, not a call. `history.changed_at` CHECKs a full
// ISO-8601 Z timestamp, so a load that read the clock itself would produce a
// different store on every run and INV-2's re-run comparison could not be
// written. The caller stamps once per migration, not once per row.
struct Options {
    QString changedAt;           // "YYYY-MM-DDTHH:MM:SSZ"; required
    QString projectRoot;         // canonical root for `project.root` (INV-8)
    bool    dryRun = false;      // plan the writes, roll back instead of commit
};

// The whole surface. One plan, one project, one transaction (§ 2.5).
// `store` must be open on an Access::Bulk connection (§ 2.2); a load on an
// Interactive one is REFUSED rather than run slowly (INV-12).
Outcome load(RoadmapStore &store, const RoadmapMigrate::MigrationPlan &plan,
             const Options &opts);

}  // namespace RoadmapMigrateLoad
```

### 2.2 The connection, and why the profile is checked rather than chosen

`RoadmapStore` takes its `Access` at construction and ANTS-3756 § 2.5 gives `Access::Bulk` a 30 s busy deadline and a 16 MiB page cache. `load()` therefore does **not** open a connection — it takes an open store, so a caller migrating ten projects opens **one** long-lived `Bulk` connection and calls `load()` ten times. That is the shape RetroDB arrived at after "database is locked" under concurrent bulk jobs, and the ROADMAP bullet is explicit that the connection lifetime, not the pragmas, is what fixed it there.

It refuses an `Interactive` store rather than proceeding (INV-12). A 5 s deadline against a migration-sized transaction fails *sometimes* — under a concurrent export, on a slow disk — which is the worst available behaviour: it would make ANTS-3757's corpus load pass locally and fail on a loaded machine. This requires `Access` to be readable, which it currently is not; § 2.4 adds the accessor.

### 2.3 The transaction shape, and the one shipped decision this changes

**`putItem()` stops opening its own transaction, and the store gains explicit transaction control.** § 1.1's measurement leaves three candidates and only one survives.

| Option | Why not |
|---|---|
| Load half writes raw SQL against `db()` | Makes it the **third** producer of `item` rows, after `putItem()` and ANTS-3761's rebuild. Every future column has three writers to update and INV-21's "canonicalised at the write path" has three paths to be true of. This is the option ANTS-3767 explicitly argued against for the same columns. |
| A `bool inTransaction` flag on `ItemWrite` | Encodes a property of the *connection* as a property of one *row*. Two calls in one transaction could disagree, and the type system would not care. |
| **Explicit `begin()`/`commit()`/`rollback()` on the store; `putItem()` uses an open transaction when there is one** | Chosen. |

```cpp
// Added to RoadmapStore. The flag is a member rather than a query because
// SQLite exposes autocommit state through sqlite3_get_autocommit(), which
// QSqlDatabase does not surface.
bool begin(QString *error = nullptr);     // BEGIN IMMEDIATE; refuses if already open
bool commit(QString *error = nullptr);
bool rollback(QString *error = nullptr);
bool inTransaction() const;
```

`putItem()` keeps its current behaviour when no transaction is open — it wraps its item-plus-element write in one, because INV-20 (exactly one `kind='item'` element per item) depends on those two inserts being atomic, and every existing caller relies on it. When a transaction *is* open it participates in the caller's, and INV-20 holds a fortiori: the enclosing transaction is wider, not narrower. **This is a change to ANTS-3756's shipped surface and is amended there, not merely described here** (§ 7): a spec that says `putItem()` self-commits while the code takes a caller's transaction is exactly the drift the review gate exists to stop.

The nesting refusal in `begin()` is deliberate and is the one behaviour worth stating twice: a `begin()` that silently no-oped inside an open transaction would make a caller's `commit()` end a transaction it did not start, which is the failure mode the SQLite CLI demonstrates in § 1.1 — the inner `COMMIT` there committed the outer transaction's rows.

### 2.4 What the store owes this half

Four writers, all of them thin, all of them additions rather than corrections. Each canonicalises its JSON on the way in for the reason ANTS-3756 § 2.3 gives — the export copies these bytes rather than transforming them.

```cpp
// section.intro — addSection() has no argument for it today. A separate
// setter rather than a wider addSection(): the intro is prose that § 2.11 of
// the read half may leave empty, and three of four call sites do not have one.
bool setSectionIntro(qint64 sectionId, const QString &intro, QString *error = nullptr);

// element rows that are NOT items. The CHECK pairs kind with payload
// (payload IS NULL exactly when kind='item'), so this refuses kind='item'
// and putItem() stays the only way an item is filed (INV-20).
bool addElement(qint64 sectionId, int position, const QString &kind,
                const QString &payload, QString *error = nullptr);

// project.legend — one JSON object per project (roadmap-data-model.md § 5.1).
bool setLegend(qint64 projectId, const QJsonObject &legend, QString *error = nullptr);

// id_prefix high-water. Advances only upward: a migration that allocated
// ANTS-0042 must never let a later run reissue it (§ 2.8).
bool raiseIdHighWater(qint64 projectId, const QString &prefix, qint64 highWater,
                      QString *error = nullptr);

// Re-run matching (§ 2.6) — resolve an id within one project, folded.
std::optional<qint64> findItem(qint64 projectId, const QString &id) const;

// § 2.2's profile check.
Access access() const;
```

### 2.5 Per-project atomicity

**One project is one transaction. A project either migrates completely or not at all.** `load()` opens the transaction, writes the project row, its sections, its items and elements, its legend and its id-prefix high-water, and commits; any failure rolls the whole thing back and returns `ok = false` with the first error. `dryRun` runs the identical path and rolls back at the end, so a dry run exercises every constraint the real one does rather than a cheaper approximation of them.

Atomicity is **per project and not per corpus**, which is a decision rather than a convenience: the corpus is ten independent projects (ANTS-3757 § 1.1), one bad `ROADMAP.md` should not deny the other nine their migration, and a single transaction spanning all ten holds the write lock for its whole duration against a 30 s deadline that other writers share. The cost is that a partial cutover is a *reachable* state, which is why § 2.10 makes it a legible one.

### 2.6 Re-run matching

A re-run is the normal case, not the exception: a source file is edited between runs and migration is re-run to pick it up. Matching is on **`(project_id, id_fold)`** — the store's own identity, case-folded within a project by ANTS-3756 INV-3 — via `findItem()`. Never on headline, never on position: both change while an item stays itself, and matching on either would delete and re-create the item, destroying its `history` rows.

For a matched item, each field the plan carries is compared with what is stored and written only if it differs (`Outcome::itemsUpdated` versus `itemsUnchanged`). Three rules make that comparison well-defined:

- **The plan is authoritative only for the fields it carries.** ANTS-3757 § 2.1.1 lists which those are, and `priority`, `resolution`, `milestone`, `visibility` and the dates are not among them — a source file cannot express them. A re-run therefore never clears a field a human set through `roadmap_log`, which is the single most destructive thing a re-run could do.
- **`provenance` is merged, not replaced.** A field this run did not write keeps the provenance it had; a field it wrote takes the plan's (`asserted`/`defaulted`/`migrated` per ANTS-3757 § 2.7–2.9).
- **An empty plan value does not overwrite a non-empty stored value** — with one exception, `body`, which is the only field whose emptiness in source is meaningful (a bullet whose continuation lines were deleted). The asymmetry is stated because a reader will otherwise read the general rule as covering it.

**Ordering is rebuilt, not diffed.** The `element` table has `UNIQUE (section_id, position)` and SQLite enforces it per statement, so shifting positions in place collides against rows that are about to move. Inside the transaction, `load()` deletes the project's `element` rows and re-inserts them from the plan. Items survive that — deleting an element row does not touch the `item` it references — and every surviving item is re-filed before commit, so INV-20 holds at every commit boundary even though it is transiently false inside the transaction. That is a property of the transaction, not a loophole: nothing outside it can observe the intermediate state.

### 2.7 An item in the store and absent from source

**It is never deleted, and it is never silently kept.** The item retains its row, its history and its identity; it is re-filed at the end of its section, its status is left exactly as it was, and it is counted in `itemsOrphaned` and reported with the `orphaned_item` note code naming its id.

Deleting would be wrong in both directions. An id absent from source is far more often a **rename or an archive rotation** (`roadmap-format.md` § 3.9 moves closed minors out of `ROADMAP.md`, which ANTS-3757 § 5 excludes from the plan, so *every archived item looks deleted to this half*) than a real deletion — and the store is primary (ANTS-3756 § 1), so it holds `history` rows and relationships that the source file never contained and cannot restore. Marking it `dropped` instead would be no better: `dropped` is an author's statement about the work, and inferring it from a file edit puts a claim in the store that no author made.

The consequence is stated rather than hidden: after the archive rotation ANTS-3766 addresses, a re-run reports every archived item as orphaned. That is noise, not damage, and ANTS-3766 removes it at the source.

### 2.8 Id allocation

ANTS-3757 § 2.9 leaves the obligation on the item — `PlannedItem::idAllocationOwed` and `closed` — because a pure planner cannot hold a counter. `load()` allocates **inside the same transaction as the write**, which is what makes the counter safe: two migrations racing on one store cannot both take `ANTS-0042`, because the second blocks on `BEGIN IMMEDIATE`.

The policy is `roadmap-data-model.md` § 7.2's and is not restated here. What this half adds is the mechanics: the prefix comes from the project's existing ids, the starting high-water from `id_prefix` (or from the maximum id in the plan when the row does not exist yet), each allocation increments it, and `raiseIdHighWater()` writes the final value before commit. `provenance.id` is `migrated` for every allocated id, per ANTS-3757 § 2.9.

**A rolled-back load allocates nothing**, which is the whole reason allocation is inside the transaction rather than before it: an id burnt by a failed run is an id that exists in no document and blocks a future one.

### 2.9 History

The **initial** load of an item writes no `history` rows — there is no prior value to record, and manufacturing one would put a change in the audit trail that never happened. A **re-run update** writes one row per changed field, with `changed_at` from `Options::changedAt` and `seq` ordered within the item, so the store can say what a source edit changed and when it was picked up.

`appendHistory()` already fails-and-reports at the store-wide cap while the item write it accompanies succeeds (ANTS-3756 INV-14). This half does **not** override that: a history write that fails at the cap does not roll the project back. Losing the migration of a project because its audit trail is full inverts the priority — the item is the data, the history is the record of it — and the failure is reported through `Outcome::notes` rather than swallowed.

### 2.10 The cutover interim

Some projects are migrated and others are not, for as long as the rollout takes, and **`project.root` is the marker** — a project row exists exactly when that project has been loaded, and it carries the canonical root that ANTS-3756 INV-8 keys on. No schema change, no `migrated_at` column, no `PRAGMA user_version` bump: the fact is already recorded by the data.

That works because per-project atomicity (§ 2.5) makes "half a project" unreachable. A project row that exists is a project whose whole plan committed, so the marker cannot be observed in a partial state — which is exactly the property a `migrated_at` column would have had to be maintained by hand to provide.

What consumes the marker is ANTS-3758, not this half: a read verb asks whether the caller's project has a row and falls back to `roadmap_query`'s markdown path when it does not. This spec states the marker and its guarantee; it does not build the fallback.

### 2.11 The report

`Outcome` is a value and every count in it is assertable, for the reason ANTS-3757 § 2.10 gives: a migration that reports to stderr is a migration whose behaviour no test can pin. The plan's own notes are carried through unchanged and joined by the codes only a load can raise:

| Code | Raised when |
|---|---|
| `orphaned_item` | § 2.7 — in the store, absent from source. Detail names the id. |
| `id_allocated` | § 2.8 — an id-less item was given one. Detail names the new id. |
| `history_capped` | § 2.9 — `appendHistory()` refused at the store-wide cap. |
| `field_conflict` | § 2.6 — the plan and the store disagree on a field the plan is **not** authoritative for. Reported, never written. |
| `project_refused` | § 2.5 — the whole project rolled back. Detail is `Outcome::error`. |

## 3. Invariants

- **INV-1** — **One project is one transaction.** A `load()` that fails at any point leaves the store byte-identical to its state before the call: no project row, no sections, no items, no id-prefix advance. *Test:* `roadmap_migrate_load` loads a plan whose Nth item violates a CHECK (an off-enum `status`), then asserts every table is empty and `ok == false`. *Breaks when:* the writes run outside a transaction — the shipped `putItem()` shape, which commits each item as it goes and leaves N−1 rows behind.
- **INV-2** — **A re-run over an unchanged source changes nothing.** Loading the same plan twice produces `itemsInserted == 0` and `itemsUpdated == 0` on the second run, and no new `history` rows. *Test:* `roadmap_migrate_load` runs `load()` twice with one `Options::changedAt`, asserting the counts and `SELECT COUNT(*) FROM history == 0`. *Breaks when:* matching is on anything but `(project_id, id_fold)`, or a field comparison treats "absent from plan" as "set to empty" — both of which rewrite every item on every run and fill `history` with changes that did not happen.
- **INV-3** — **A re-run never clears a field the plan does not carry.** An item whose `priority`, `milestone`, `resolution` or `visibility` was set through the store keeps it across a re-run that does not mention it. *Test:* `roadmap_migrate_load` loads, sets `priority` via `setItemField()`, re-loads, asserts it survives. *Breaks when:* the update path writes a full `ItemWrite` rather than the differing fields — the obvious implementation, and the one that silently destroys human edits.
- **INV-4** — **An item absent from source is retained, re-filed and reported.** Its row, its `history` and its status survive; it has exactly one `element` row after the re-run; `itemsOrphaned` counts it and an `orphaned_item` note names it. *Test:* `roadmap_migrate_load` loads two items, re-loads a plan carrying only the first, asserts the second's row and history survive and that both items still satisfy INV-20. *Breaks when:* the element rebuild (§ 2.6) re-inserts only the plan's items, which leaves the orphan unfiled and INV-20 false at a commit boundary.
- **INV-5** — **Ordering is rebuilt without a UNIQUE collision.** Re-loading a plan whose items are in a different order succeeds, and afterwards each section's positions are exactly `0..n-1` with no gaps and no duplicates. *Test:* `roadmap_migrate_load` loads a three-item section, re-loads it reversed, asserts the order and that `SELECT COUNT(*) FROM element` is unchanged. *Breaks when:* positions are updated in place — `UNIQUE (section_id, position)` fires against a row that has not moved yet.
- **INV-6** — **A rolled-back load allocates no id.** After a failing load of a plan containing id-less items, `id_prefix` holds no row for the project (or its prior value, on a re-run). *Test:* `roadmap_migrate_load` fails a load carrying `idAllocationOwed` items and asserts the high-water. *Breaks when:* allocation runs before the transaction, which burns ids that appear in no document.
- **INV-7** — **`begin()` refuses to nest.** Calling it inside an open transaction fails and reports rather than no-oping. *Test:* `roadmap_store_schema` asserts the second `begin()` returns false and that a subsequent `commit()` still commits the first one's writes. *Breaks when:* it no-ops, so a caller's `commit()` ends a transaction it did not open — the exact behaviour § 1.1's CLI transcript shows.
- **INV-8** — **`putItem()` is atomic with or without an enclosing transaction.** With none it self-commits; with one it participates and writes nothing durable until the caller commits. Both leave exactly one `kind='item'` element per item. *Test:* `roadmap_store_schema`, two legs — the existing standalone case, and one where a `putItem()` inside a rolled-back transaction leaves no item **and** no element row. *Breaks when:* the transaction check reads the connection rather than the store's own flag, or the element insert is moved outside it.
- **INV-9** — **The three writers § 2.4 adds store canonical JSON.** `setLegend()` and the plan's `table` payloads through `addElement()` produce the same bytes ANTS-3761's export would. *Test:* `roadmap_migrate_load` writes an out-of-order legend object and a table payload, asserting the stored text is sorted and compact. *Breaks when:* either binds `QJsonDocument::toJson(Compact)` — the ANTS-3767 failure, one column further along.
- **INV-10** — **`addElement()` cannot file an item.** A call with `kind = 'item'` is refused, so `putItem()` remains the only path by which an item acquires an element row. *Test:* `roadmap_store_schema` asserts the refusal. *Breaks when:* the writer passes `kind` through to the INSERT and lets the DDL CHECK decide, which admits a `kind='item'` row with a NULL `item_pk`… and the CHECK catches that one, but not a well-formed second filing of an already-filed item.
- **INV-11** — **A project row exists exactly when that project's plan committed.** After a failed load there is no row; after a successful one there is exactly one, keyed on the canonical root. *Test:* `roadmap_migrate_load` asserts both directions. *Breaks when:* the project row is written before the transaction, or outside it — which makes the § 2.10 cutover marker claim a migration that did not happen.
- **INV-12** — **A load against an `Interactive` store is refused.** *Test:* `roadmap_migrate_load` constructs an `Interactive` store and asserts `ok == false` with nothing written. *Breaks when:* the profile is not checked — the load then works on a quiet machine and fails at 5 s against a concurrent export, which is worse than failing always.
- **INV-13** — **`dryRun` writes nothing and reports what a real run would have done.** The counts of a dry run equal those of the real run that follows it, and the store is unchanged after the dry run. *Test:* `roadmap_migrate_load` compares the two `Outcome`s field by field. *Breaks when:* the dry run short-circuits before the constraint-bearing writes, which makes it a syntax check wearing an atomicity check's clothes.

## 4. RAM / build cost

**Memory.** One plan at a time, and ANTS-3757 § 4 already budgets the plan itself; this half adds the `Bulk` connection's **16 MiB** page cache (ANTS-3756 § 2.5, `kBulkCacheKiB`) and one `findItem()` result at a time. It deliberately does **not** build an in-memory map of the project's existing items: the corpus's largest project is this one, the lookup is an indexed point query on `(project_id, id_fold)`, and a map would trade a bounded cost for one that grows with the corpus. Ten projects migrated through one connection cost one 16 MiB cache, not ten, which is the second reason § 2.2 takes an open store rather than opening its own.

**Build.** Two new files in an existing library, no new target, no new dependency — `ants_roadmapstore_lib` already links `Qt6::Sql`. The feature test joins `test_core`'s `SOURCES` per `tests/features/README.md`; ANTS-1217 consolidated 141 standalone binaries into seven bundles to bound build-time RAM and a new binary would reverse it.

## 5. Out of scope

- **Reading, parsing and planning** — [ANTS-3757](ANTS-3757-roadmap-migration-read.md). This half never opens a `ROADMAP.md`.
- **The read verbs, the published render, and the fate of `roadmap_query` / `roadmap_log` / `RoadmapDialog`** — ANTS-3758. § 2.10 defines the cutover marker; consuming it is that id's.
- **Rotated archives** — ANTS-3766, inherited from ANTS-3757 § 5. § 2.7 states the consequence for orphan reporting in the meantime.
- **Deleting anything from the store.** Not deferred — § 2.7 decides against it, and a later id that wants deletion is proposing a policy this spec argues with rather than completing work it left.
- **A CLI or MCP verb that drives the migration.** This is a library surface; who calls it, and whether a user can trigger it, is ANTS-3758's.
- **Migrating the store's own schema.** `user_version` stays 1: the store has never shipped data, so there is nothing to migrate from.

## 6. Tests

Feature tests under `tests/features/`, label `features;fast`:

| Directory | Covers |
|---|---|
| `roadmap_migrate_load/` | INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-9, INV-11, INV-12, INV-13 |
| `roadmap_store_schema/` | INV-7, INV-8, INV-10 — the store-surface changes, filed with the rest of ANTS-3756's write-path invariants rather than in a second directory |

All **thirteen** invariants are covered; none is a grep-only check. The new directory adds its `test_*.cpp` to **`test_core`**'s `SOURCES` list, per `tests/features/README.md` step 4 and for the ANTS-1217 reason § 4 gives; no `add_executable`.

Per the project convention (`CLAUDE.md`, `testing.md`), each test is verified to **fail against pre-implementation source** before the implementation is restored. Two are worth naming because the mutation is not the obvious one: INV-2's re-run must be shown red against a loader that rewrites every field on every run (which passes a load-once test), and INV-5's must be shown red against in-place position updates (which pass on a plan whose order did not change).

## 7. Cross-doc impact

- **[ANTS-3756](ANTS-3756-roadmap-store-schema.md) is amended, not merely referenced.** § 2.3 changes `putItem()`'s shipped transaction behaviour and § 2.4 adds six methods to the store's public surface. That spec's § 2 and its INV-20 discussion both describe `putItem()` as opening its own transaction, which stops being unconditionally true. INV-7, INV-8 and INV-10 above are store invariants and are folded into ANTS-3756's own list at implementation, since a store invariant filed only in a migration spec is one nobody looking at the store will find.
- **[ANTS-3757](ANTS-3757-roadmap-migration-read.md)** — § 2.1.1's last two owed rows (`projectId`, `sectionId`) are discharged here; its § 7 is updated when this ships.
- **[ANTS-3761](ANTS-3761-roadmap-export-format.md)** — the export must round-trip everything this writes. The `element` rows, `section.intro` and `project.legend` are already in its record set; this is the first writer to produce them from anything but a rebuild, so its INV-2 column diff becomes a real test of this half rather than of the rebuild alone.
- **`docs/standards/roadmap-data-model.md`** — § 7.2's allocation policy is executed here and not restated. No amendment: § 2.8 adds mechanics, not policy.
- **`CLAUDE.md`** — the module map gains `src/roadmapmigrateload.{h,cpp}`; `docs/subsystems.md`'s roadmap lane gains the same.
- **`ROADMAP.md`** — ANTS-3765 flips to in-progress at implementation; ANTS-3758 unblocks on ship.
- **`CHANGELOG.md`** — on ship.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
