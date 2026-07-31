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

2. **Four things the plan carries have no writer at all.** `PlannedSection::intro`, `PlannedElement` (`kind` `narration` or `table`), `PlannedLegend`, and the `id_prefix` high-water mark § 2.8 must advance. The store's public **writers** are `registerProject`, `addSection`, `putItem`, `setItemField`, `relateItems`, `relateCrossProject` and `appendHistory` (`src/roadmapstore.h`, which also exposes `defaultPath`, `open`, `isOpen`, `createdSchema`, `path`, `db`, `historyBytes`, `historyCapBytes` and `canonicalJson`) — `addSection()` takes no `intro` argument, and there is no element, legend or id-prefix writer among them. ANTS-3761's rebuild reaches these columns only by writing raw SQL against `RoadmapStore::db()`, which is already a second producer for the `item` row; a third would be worse. § 2.4 adds the writers instead.

3. **There is almost no read path, and a re-run is mostly reading.** Nothing on the store answers "does this project already have an item with this id?", "what does that item currently hold?", "which section is this slug?" or "what is the id high-water?" — and § 2.6 needs all four before it may write anything. `db()` is the only way in today. § 2.4 adds the readers.

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
    // (§ 2.11's codes). Never a SUBSET of the plan's notes — a plan note is
    // never dropped — so one report covers the whole migration of one project.
    QVector<RoadmapMigrate::Note> notes;
};

// The clock is a PARAMETER, not a call. `history.changed_at` CHECKs a full
// ISO-8601 Z timestamp, so a load that read the clock itself would produce a
// different store on every run and INV-2's re-run comparison could not be
// written. The caller stamps once per migration, not once per row.
struct Options {
    // Required, and validated BEFORE the transaction opens: `history.changed_at`
    // CHECKs this exact shape, so an ill-formed stamp would otherwise surface as
    // a rolled-back project at the first re-run update rather than as a refusal.
    // A malformed value refuses with the `bad_options` note and writes nothing.
    QString changedAt;           // "YYYY-MM-DDTHH:MM:SSZ"
    // Passed to registerProject(), WHICH CANONICALISES IT (ANTS-3756 INV-8) —
    // the caller supplies the root it was given, not a pre-canonicalised path.
    // A root that cannot be canonicalised is that method's refusal, not this
    // one's, and it aborts the load like any other write failure.
    QString projectRoot;
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

| Option | Verdict |
|---|---|
| Load half writes raw SQL against `db()` | **No.** Makes it the **third** producer of `item` rows, after `putItem()` and ANTS-3761's rebuild. Every future column has three writers to update and INV-21's "canonicalised at the write path" has three paths to be true of. This is the option ANTS-3767 explicitly argued against for the same columns. |
| A `bool inTransaction` flag on `ItemWrite` | **No.** Encodes a property of the *connection* as a property of one *row*. Two calls in one transaction could disagree, and the type system would not care. |
| `QSqlDatabase::transaction()` / `commit()` / `rollback()`, which already exist on the store's `m_db` | **No**, and this is the one an implementer reaches for first, so it is rejected on the record rather than by omission. Qt issues a **deferred** `BEGIN`; the store's whole concurrency design is `BEGIN IMMEDIATE` (ANTS-3756 § 2.5, INV-16), and a deferred transaction takes its write lock at the first write — so it can fail with `SQLITE_BUSY` *mid-load*, after work has been done, and § 2.8's "two migrations cannot both take ANTS-0042" argument depends on the lock being held from the start. |
| **Explicit `begin()`/`commit()`/`rollback()` on the store; `putItem()` uses an open transaction when there is one** | **Chosen.** |

```cpp
// Added to RoadmapStore. The flag is a member rather than a query because
// SQLite exposes autocommit state through sqlite3_get_autocommit(), which
// QSqlDatabase does not surface.
bool begin(QString *error = nullptr);     // BEGIN IMMEDIATE; refuses if already open
bool commit(QString *error = nullptr);    // refuses when none is open
bool rollback(QString *error = nullptr);  // refuses when none is open
bool inTransaction() const;
```

`begin()` issues `BEGIN IMMEDIATE`, never a deferred `BEGIN`, for the reason the table's third row gives. `commit()` and `rollback()` **refuse when no transaction is open** rather than returning success: a silent no-op there is the same class of defect as a nesting `begin()` — the caller believes it has ended a transaction and has not.

`putItem()` keeps its current behaviour when no transaction is open — it wraps its item-plus-element write in one, because INV-20 (exactly one `kind='item'` element per item) depends on those two inserts being atomic, and every existing caller relies on it. When a transaction *is* open it participates in the caller's, and INV-20 holds a fortiori: the enclosing transaction is wider, not narrower.

**Its three internal failure paths stop issuing `ROLLBACK` when they are not the transaction's owner, and this is the sharpest edge in the change.** As shipped, `putItem()` calls `exec(m_db, "ROLLBACK", nullptr)` on each of its three failure paths — the section-project check, the item insert, the element insert. Left unchanged inside an enclosing transaction, one bad item would abort the **caller's** transaction from the inside; `load()` would carry on believing it was still in one, every subsequent write would run in autocommit and **persist**, and `Outcome` would report a clean partial load. That is INV-1 inverted while every count says success — the worst available failure, because nothing observes it. So: when it owns the transaction it rolls back exactly as today; when it does not, it returns `std::nullopt` with the error set and touches the transaction state not at all, leaving the unwind to `load()`. INV-8 asserts both halves.

**This is a change to ANTS-3756's shipped surface and is amended there, not merely described here** (§ 7): a spec that says `putItem()` self-commits while the code takes a caller's transaction is exactly the drift the review gate exists to stop.

The nesting refusal in `begin()` is deliberate and is the one behaviour worth stating twice: a `begin()` that silently no-oped inside an open transaction would make a caller's `commit()` end a transaction it did not start, which is the failure mode the SQLite CLI demonstrates in § 1.1 — the inner `COMMIT` there committed the outer transaction's rows.

### 2.4 What the store owes this half

**Eleven methods and one struct, and the list is exhaustive** — § 2.3 adds four more, so **fifteen** methods land on ANTS-3756 in total (§ 7). All are additions rather than corrections, and the five *readers* are as load-bearing as the writers: a re-run cannot decide anything it cannot first read, and the shipped surface has no reader at all beyond `db()`.

**Only two of these canonicalise JSON**, and stating it per method rather than as a blanket sentence is deliberate: `setSectionIntro()` writes prose, `raiseIdHighWater()` writes an integer, and `addElement()` with `kind = 'narration'` writes prose that ANTS-3756 § 2.3 **forbids** canonicalising — "canonicalising prose as JSON is undefined rather than merely wasteful". A blanket rule here would have an implementer corrupt every narration payload while following the spec exactly.

```cpp
// --- writers ---

// section.intro — addSection() has no argument for it today. A separate
// setter rather than a wider addSection(): the intro is prose the read half
// may leave empty, and three of four call sites do not have one.
// Stored VERBATIM; not JSON.
bool setSectionIntro(qint64 sectionId, const QString &intro, QString *error = nullptr);

// element rows that are NOT items. The CHECK pairs kind with payload
// (payload IS NULL exactly when kind='item'), so this refuses kind='item'
// and putItem()/fileItem() stay the only ways an item is filed (INV-10, INV-20).
// payload is canonicalised when kind='table' and stored VERBATIM when
// kind='narration' (ANTS-3756 § 2.3).
bool addElement(qint64 sectionId, int position, const QString &kind,
                const QString &payload, QString *error = nullptr);

// The re-filing writer § 2.6's element rebuild needs, and the piece whose
// absence made that rebuild unbuildable: putItem() files a NEW item, and an
// item that already exists cannot be re-inserted (UNIQUE (project_id, id_fold)).
// Writes one kind='item' element row for an item that currently has none.
// Refuses if the item is already filed — INV-20's "at most one" is the partial
// index, and this must not be the way round it.
bool fileItem(qint64 itemPk, qint64 sectionId, int position, QString *error = nullptr);

// project.legend — one JSON object per project (roadmap-data-model.md § 5.1).
// CANONICALISED.
bool setLegend(qint64 projectId, const QJsonObject &legend, QString *error = nullptr);

// id_prefix high-water. Advances only upward: a migration that allocated
// ANTS-0042 must never let a later run reissue it (§ 2.8). A value at or below
// the stored one is a no-op, not an error.
bool raiseIdHighWater(qint64 projectId, const QString &prefix, qint64 highWater,
                      QString *error = nullptr);

// setItemField() records provenance `asserted` for whatever it writes, which is
// right for a human edit and wrong for every write this half makes: migration
// records `migrated` and `defaulted` (ANTS-3757 § 2.7–2.9), and losing that
// distinction collapses the migrated-versus-asserted difference the model's
// § 3.1 write-tier gate rests on. So the provenance value becomes a parameter.
// The existing 3-argument overload keeps its meaning — `asserted` — so no
// existing call site changes and ANTS-3756 INV-10 is untouched.
bool setItemField(qint64 itemPk, const QString &field, const QString &value,
                  const QString &provenance, QString *error = nullptr);

// --- readers ---

// Re-run matching (§ 2.6) — resolve an id within one project, folded.
// The error out-param is not decoration: without it `nullopt` conflates "no
// such item" with "the query failed", and the failure path of that confusion
// is an INSERT of an item that already exists.
std::optional<qint64> findItem(qint64 projectId, const QString &id,
                               QString *error = nullptr) const;

// § 2.6's field comparison. Returns the stored item as an ItemWrite (the same
// shape the writer takes, so "compare then write the difference" needs no
// second type) plus its current filing, which § 2.7 must capture BEFORE the
// element rebuild deletes it.
struct ItemRead { RoadmapStore::ItemWrite fields; qint64 sectionId; int position; };
std::optional<ItemRead> readItem(qint64 itemPk, QString *error = nullptr) const;

// Section resolution on a re-run. addSection() is a bare INSERT and collides on
// UNIQUE (project_id, slug) the second time, so a re-run MUST resolve first.
// registerProject() needs no equivalent: it is already get-or-create, selecting
// on `root` before inserting (verified in shipped source, 2026-07-31).
std::optional<qint64> findSection(qint64 projectId, const QString &slug,
                                  QString *error = nullptr) const;

// § 2.8's starting point. Absent row ⇒ nullopt, which is not an error.
std::optional<qint64> idHighWater(qint64 projectId, const QString &prefix,
                                  QString *error = nullptr) const;

// § 2.2's profile check.
Access access() const;
```

### 2.5 Per-project atomicity

**One project is one transaction. A project either migrates completely or not at all.** `load()` opens the transaction, writes the project row, its sections, its items and elements, its legend and its id-prefix high-water, and commits; any failure rolls the whole thing back and returns `ok = false` with the first error.

**One exception, and it is stated here rather than only in § 2.9 because a rule with an exception in another section is a rule readers apply wrongly.** A `history` write refused at the store-wide cap does **not** roll the project back: `ok` stays `true`, the item write it accompanies stands, and a `history_capped` note carries it. § 2.9 argues why. Every *other* failure — including a `history` write that fails for any reason other than the cap — aborts the project. `dryRun` runs the identical path and rolls back at the end, so a dry run exercises every constraint the real one does rather than a cheaper approximation of them.

Atomicity is **per project and not per corpus**, which is a decision rather than a convenience: the corpus is ten independent projects (ANTS-3757 § 1.1), one bad `ROADMAP.md` should not deny the other nine their migration, and a single transaction spanning all ten holds the write lock for its whole duration against a 30 s deadline that other writers share. The cost is that a partial cutover is a *reachable* state, which is why § 2.10 makes it a legible one.

### 2.6 Re-run matching

A re-run is the normal case, not the exception: a source file is edited between runs and migration is re-run to pick it up. Matching is on **`(project_id, id_fold)`** — the store's own identity, case-folded within a project by ANTS-3756 INV-3 — via `findItem()`. Never on headline, never on position: both change while an item stays itself, and matching on either would delete and re-create the item, destroying its `history` rows.

**The project and its sections are resolved before anything is written.** `registerProject()` is already get-or-create — it selects on `root` and returns the existing `project_id` — so it needs no change and a re-run reuses the same project row. `addSection()` is **not**: it is a bare INSERT and collides on `UNIQUE (project_id, slug)` the second time, which would abort every re-run at its first section. So a re-run resolves each section with `findSection()` and calls `addSection()` only for a slug that is genuinely new, updating `intro`, `title` and `level` on the ones that exist. **A section in the store and absent from the plan is retained, not deleted** — for § 2.7's reason, one level up: it may hold an orphaned item, and deleting it would orphan that item's filing rather than its row.

For a matched item, each field the plan carries is compared with what `readItem()` returns and written only if it differs (`Outcome::itemsUpdated` versus `itemsUnchanged`). Three rules make that comparison well-defined:

- **The plan is authoritative only for the fields it carries.** ANTS-3757 § 2.1.1 lists which those are, and `priority`, `resolution`, `milestone`, `visibility` and the dates are not among them — a source file cannot express them. A re-run therefore never clears a field a human set through `roadmap_log`, which is the single most destructive thing a re-run could do.
- **`provenance` is merged, not replaced.** A field this run did not write keeps the provenance it had; a field it wrote takes the plan's (`asserted`/`defaulted`/`migrated` per ANTS-3757 § 2.7–2.9). This is reachable only through § 2.4's four-argument `setItemField()`: the shipped three-argument form hardcodes `asserted`, which is correct for a human edit and wrong for every write migration makes.
- **An empty plan value does not overwrite a non-empty stored value** — with one exception, `body`, which is the only field whose emptiness in source is meaningful (a bullet whose continuation lines were deleted). The asymmetry is stated because a reader will otherwise read the general rule as covering it.

**Ordering is rebuilt, not diffed.** The `element` table has `UNIQUE (section_id, position)`, and SQLite enforces it **per row, as each row is written** — so an in-place shift fails at the first row whose new position is still held by a row that has not moved yet. Inside the transaction, `load()` therefore:

1. **captures each existing item's filing first** — `readItem()` returns `sectionId` and `position`, and once the element rows are gone the store has no other record of where an item was filed (ANTS-3756 INV-20: "there is no `item.section` column");
2. deletes the project's `element` rows;
3. re-inserts them from the plan — new items via `putItem()`, which files as it inserts, and **already-existing items via `fileItem()`**, which is the writer § 2.4 adds for exactly this and without which every matched item would end the transaction unfiled;
4. re-files every orphan (§ 2.7) before commit.

Items survive the delete — removing an element row does not touch the `item` it references — so INV-20 holds at every commit boundary even though it is transiently false inside the transaction. That is a property of the transaction, not a loophole: nothing outside it can observe the intermediate state.

### 2.7 An item in the store and absent from source

**It is never deleted, and it is never silently kept.** The item retains its row, its history and its identity; its status is left exactly as it was, and it is counted in `itemsOrphaned` and reported with the `orphaned_item` note code naming its id.

**Where it is re-filed needs stating, because § 2.6 deletes the only record of where it was.** Its `(sectionId, position)` is captured by `readItem()` before the delete, and it is re-filed **at the end of that same section**, after every item the plan placed there. When that section is itself absent from the new plan the section row survives (§ 2.6) and the orphan stays in it. When the section row does not exist at all — a store written by an earlier tool, or a plan that renamed every slug — the orphan is filed under the synthetic root section, which the store already accepts (empty slug and title, level 0) and which ANTS-3757 § 2.1 gives the plan for content above the first heading.

Deleting would be wrong in both directions. An id absent from source is far more often a **rename or an archive rotation** (`roadmap-format.md` § 3.9 moves closed minors out of `ROADMAP.md`, which ANTS-3757 § 5 excludes from the plan, so *every archived item looks deleted to this half*) than a real deletion — and the store is primary (ANTS-3756 § 1), so it holds `history` rows and relationships that the source file never contained and cannot restore. Marking it `dropped` instead would be no better: `dropped` is an author's statement about the work, and inferring it from a file edit puts a claim in the store that no author made.

The consequence is stated rather than hidden: after the archive rotation ANTS-3766 addresses, a re-run reports every archived item as orphaned. That is noise, not damage, and ANTS-3766 removes it at the source.

### 2.8 Id allocation

ANTS-3757 § 2.9 leaves the obligation on the item — `PlannedItem::idAllocationOwed` and `closed` — because a pure planner cannot hold a counter. `load()` allocates **inside the same transaction as the write**, which is what makes the counter safe: two migrations racing on one store cannot both take `ANTS-0042`, because the second blocks on `BEGIN IMMEDIATE`.

The policy is `roadmap-data-model.md` § 7.2's and is not restated here. What this half adds is the mechanics, and each step needs a rule rather than an intuition — "the maximum id" is not defined over `TEXT`:

1. **The prefix.** From the project's existing `id_prefix` row when it has one. Failing that, from the most frequent prefix among the plan's `idOrigin == "parsed"` ids — parsed only, because a `quarantined` id is off-grammar by definition (ANTS-3756 INV-4) and `[Cl9]` would otherwise contribute the prefix `Cl`. Failing *that* — a project with no id-bearing items at all, which is the first-run case for several of the corpus's ten — the caller's `exportSlug` uppercased, which is how `roadmap_log` derives one today.
2. **The starting high-water.** `idHighWater()` when the row exists. Otherwise the maximum **numeric suffix** among the plan's parsed ids carrying the chosen prefix — parsed as the digits after the final `-`, ignoring any id whose suffix does not parse. Quarantined and synthesised (`PASS-N-M`) ids are excluded from both steps: neither comes from the counter, and a `PASS-43-5` would otherwise set the high-water to 5.
3. Each allocation increments it; `raiseIdHighWater()` writes the final value before commit.

`provenance.id` is `migrated` for every allocated id, per ANTS-3757 § 2.9.

**A rolled-back load allocates nothing**, which is the whole reason allocation is inside the transaction rather than before it: an id burnt by a failed run is an id that exists in no document and blocks a future one.

### 2.9 History

The **initial** load of an item writes no `history` rows — there is no prior value to record, and manufacturing one would put a change in the audit trail that never happened. A **re-run update** writes one row per changed field, with `changed_at` from `Options::changedAt`, so the store can say what a source edit changed and when it was picked up.

**`seq` continues from the stored maximum for that `(item_pk, changed_at)`, and does not restart at zero.** `history` carries `UNIQUE (item_pk, changed_at, seq)`, and `changedAt` is one stamp per migration rather than per row — so two runs given the same stamp (a scripted re-run, a test, a caller that stamps once a day) collide on the second run's first row and abort the whole project. Continuing from `MAX(seq)` costs one query per updated item and removes a failure whose trigger is a caller doing something entirely reasonable.

`appendHistory()` already fails-and-reports at the store-wide cap while the item write it accompanies succeeds (ANTS-3756 INV-14). This half does **not** override that: a history write that fails at the cap does not roll the project back. Losing the migration of a project because its audit trail is full inverts the priority — the item is the data, the history is the record of it — and the failure is reported through `Outcome::notes` rather than swallowed.

### 2.10 The cutover interim

Some projects are migrated and others are not, for as long as the rollout takes, and **`project.root` is the marker** — a project row exists exactly when that project has been loaded, and it carries the canonical root that ANTS-3756 INV-8 keys on. No schema change, no `migrated_at` column, no `PRAGMA user_version` bump: the fact is already recorded by the data.

That works because per-project atomicity (§ 2.5) makes "half a project" unreachable. A project row that exists is a project whose whole plan committed, so the marker cannot be observed in a partial state — which is exactly the property a `migrated_at` column would have had to be maintained by hand to provide.

What consumes the marker is ANTS-3758, not this half: a read verb asks whether the caller's project has a row and falls back to `roadmap_query`'s markdown path when it does not. This spec states the marker and its guarantee; it does not build the fallback.

### 2.11 The report

`Outcome` is a value and every count in it is assertable, for the reason ANTS-3757 § 2.10 gives: a migration that reports to stderr is a migration whose behaviour no test can pin. The plan's own notes are carried through unchanged and joined by the codes only a load can raise.

**These six codes EXTEND ANTS-3757 § 2.10's set, which that spec calls closed.** Naming them here without saying so would leave two documents each claiming to hold the whole vocabulary — § 7 records the amendment.

| Code | Raised when |
|---|---|
| `orphaned_item` | § 2.7 — in the store, absent from source. Detail names the id. |
| `id_allocated` | § 2.8 — an id-less item was given one. Detail names the new id. |
| `history_capped` | § 2.9 — `appendHistory()` refused at the store-wide cap. |
| `field_conflict` | § 2.6 — the plan and the store disagree on a field the plan is **not** authoritative for. Reported, never written. |
| `project_refused` | § 2.5 — the whole project rolled back. Detail is `Outcome::error`. |
| `bad_options` | § 2.1 — `Options::changedAt` is absent or not an ISO-8601 Z stamp. Raised before the transaction opens, so nothing is written. |

## 3. Invariants

- **INV-1** — **One project is one transaction.** A `load()` that fails anywhere except § 2.5's one stated exception leaves **no row in any table this load would have written**: no project row, no sections, no items, no elements, no id-prefix advance. (Byte-identity of the file is deliberately *not* claimed — WAL frames and freelist pages move under a rolled-back transaction, so it would be unassertable.) *Test:* `roadmap_migrate_load` builds a plan whose Nth item violates a CHECK — an off-enum `status` — and asserts every table is empty and `ok == false`. **The test constructs the `MigrationPlan` struct directly rather than parsing markdown**, which is what makes that fault injectable: ANTS-3757 § 2.7's status vocabulary is total, so no real source file can produce an off-enum status, and a fixture-driven test could not build this case at all. *Breaks when:* the writes run outside a transaction — the shipped `putItem()` shape, which commits each item as it goes and leaves N−1 rows behind.
- **INV-2** — **A re-run over an unchanged source changes nothing.** Loading the same plan twice produces `itemsInserted == 0` and `itemsUpdated == 0` on the second run, and no new `history` rows. *Test:* `roadmap_migrate_load` runs `load()` twice with one `Options::changedAt`, asserting the counts and `SELECT COUNT(*) FROM history == 0`. *Breaks when:* matching is on anything but `(project_id, id_fold)`, or a field comparison treats "absent from plan" as "set to empty" — both of which rewrite every item on every run and fill `history` with changes that did not happen.
- **INV-3** — **A re-run never clears a field the plan does not carry.** An item whose `milestone`, `resolution`, `visibility` or `priority` was set through the store keeps it across a re-run that does not mention it. *Test:* `roadmap_migrate_load` loads, sets **`milestone`** via `setItemField()`, re-loads, asserts it survives. The field choice is load-bearing: `priority` is in neither `setItemField()`'s allowlist nor `QString`-typed, so the obvious recipe — set `priority`, re-load — cannot run at all, and `milestone` is the nearest field that is both writable and outside the plan's set. *Breaks when:* the update path writes a full `ItemWrite` rather than the differing fields — the obvious implementation, and the one that silently destroys human edits.
- **INV-4** — **An item absent from source is retained, re-filed and reported.** Its row, its `history` and its status survive; it has exactly one `element` row after the re-run; `itemsOrphaned` counts it and an `orphaned_item` note names it. *Test:* `roadmap_migrate_load` loads two items, re-loads a plan carrying only the first, asserts the second's row and history survive and that both items still satisfy INV-20. *Breaks when:* the element rebuild (§ 2.6) re-inserts only the plan's items, which leaves the orphan unfiled and INV-20 false at a commit boundary.
- **INV-5** — **Ordering is rebuilt without a UNIQUE collision.** Re-loading a plan whose items are in a different order succeeds, and afterwards each section's positions are exactly `0..n-1` with no gaps and no duplicates. *Test:* `roadmap_migrate_load` loads a three-item section, re-loads it reversed, asserts the order and that `SELECT COUNT(*) FROM element` is unchanged. *Breaks when:* positions are updated in place — `UNIQUE (section_id, position)` fires against a row that has not moved yet.
- **INV-6** — **A rolled-back load allocates no id.** After a failing load of a plan containing id-less items, `id_prefix` holds **no row** for that project on a first run, and **exactly its pre-run `high_water`** on a re-run. *Test:* `roadmap_migrate_load`, two legs asserting those two values — the first-run leg asserts `SELECT COUNT(*) FROM id_prefix == 0`, the re-run leg seeds a high-water of 41, fails a load that would have allocated, and asserts it is still 41. Stating the expected value in both branches is the point: "asserts the high-water" is satisfiable by a test that asserts nothing. *Breaks when:* allocation runs before the transaction, which burns ids that appear in no document.
- **INV-7** — **`begin()` refuses to nest.** Calling it inside an open transaction fails and reports rather than no-oping. *Test:* `roadmap_store_schema` asserts the second `begin()` returns false and that a subsequent `commit()` still commits the first one's writes. *Breaks when:* it no-ops, so a caller's `commit()` ends a transaction it did not open — the exact behaviour § 1.1's CLI transcript shows.
- **INV-8** — **`putItem()` is atomic with or without an enclosing transaction, and never rolls back one it does not own.** With no transaction open it self-commits; with one open it participates, writes nothing durable until the caller commits, and on failure returns `std::nullopt` **without issuing `ROLLBACK`**. *Test:* `roadmap_store_schema`, three legs — the existing standalone case; a `putItem()` inside a rolled-back transaction leaving no item **and** no element row; and a *failing* `putItem()` inside an open transaction after which `inTransaction()` is still true and a subsequent write is still rolled back by the caller. The third leg is the one that matters: without it, a `putItem()` that aborts the caller's transaction from the inside passes the first two while every later write silently autocommits. *Breaks when:* the shipped `ROLLBACK` calls are left on the failure paths, or the transaction check reads the connection rather than the store's own flag.
- **INV-9** — **The two JSON-writing methods § 2.4 adds store canonical JSON, and the other two store their text verbatim.** `setLegend()` and `addElement()` with `kind='table'` produce the same bytes ANTS-3761's export would; `setSectionIntro()` and `addElement()` with `kind='narration'` store prose **unchanged**. *Test:* `roadmap_migrate_load` writes an out-of-order legend object and a table payload and asserts the stored text is sorted and compact, then writes a narration payload containing `{"b":1,"a":2}` as literal prose and asserts it round-trips byte-for-byte. *Breaks when:* either JSON writer binds `QJsonDocument::toJson(Compact)` — the ANTS-3767 failure, one column further along — or the blanket reading is taken and narration prose is canonicalised, which ANTS-3756 § 2.3 calls undefined rather than merely wasteful.
- **INV-10** — **`addElement()` cannot file an item, and `fileItem()` cannot double-file one.** `addElement()` refuses `kind = 'item'` outright (it has no `item_pk` parameter, so it could not produce a well-formed filing even if it tried); `fileItem()` refuses an item that already has an element row. Together they keep `putItem()` and `fileItem()` the only paths by which an item acquires a filing. *Test:* `roadmap_store_schema` asserts both refusals. *Breaks when:* `addElement()` passes `kind` through to the INSERT and lets the DDL CHECK decide, or `fileItem()` omits its already-filed check and leans on `elem_item_uq` — which does catch it, as a constraint violation that aborts the caller's whole migration rather than a reported refusal.
- **INV-11** — **A project row exists exactly when that project's plan committed.** After a failed load there is no row; after a successful one there is exactly one, keyed on the canonical root. *Test:* `roadmap_migrate_load` asserts both directions. *Breaks when:* the project row is written before the transaction, or outside it — which makes the § 2.10 cutover marker claim a migration that did not happen.
- **INV-12** — **A load against an `Interactive` store is refused.** *Test:* `roadmap_migrate_load` constructs an `Interactive` store and asserts `ok == false` with nothing written. *Breaks when:* the profile is not checked — the load then works on a quiet machine and fails at 5 s against a concurrent export, which is worse than failing always.
- **INV-13** — **`dryRun` writes nothing and reports what a real run would have done.** Every count of a dry run equals the same count of the real run that follows it, and the store is unchanged after the dry run. *Test:* `roadmap_migrate_load` compares the two `Outcome`s **count by count, excluding `projectId`** — that is a rowid the dry run rolled back, so any equality between the two is incidental and asserting it would pass or fail on SQLite's rowid reuse rather than on this invariant. *Breaks when:* the dry run short-circuits before the constraint-bearing writes, which makes it a syntax check wearing an atomicity check's clothes.
- **INV-14** — **A re-run's `history` rows never collide.** Two loads given the **same** `Options::changedAt`, both updating the same field of the same item, produce two `history` rows rather than a constraint violation. *Test:* `roadmap_migrate_load` loads, re-loads with a changed headline and one stamp, re-loads again with a second changed headline and **the same** stamp, and asserts two history rows and `ok == true` throughout. *Breaks when:* `seq` restarts at 0 per run — `UNIQUE (item_pk, changed_at, seq)` then aborts the third load, and the trigger is a caller stamping two runs identically, which is not a misuse.

## 4. RAM / build cost

**Memory.** One plan at a time, and ANTS-3757 § 4 already budgets the plan itself; this half adds the `Bulk` connection's **16 MiB** page cache (ANTS-3756 § 2.5, `kBulkCacheKiB`) plus one `readItem()` result at a time. It deliberately does **not** build an in-memory map of the project's existing items: the lookup is an indexed point query on `(project_id, id_fold)`, and a map would trade a bounded cost for one that grows with the corpus. The per-project item counts are ANTS-3757 § 1.1's and are not restated here — that section is the single home for every corpus figure precisely because the same count drifting between documents was a finding in two of its review loops. Ten projects migrated through one connection cost one 16 MiB cache, not ten, which is the second reason § 2.2 takes an open store rather than opening its own.

**Time.** Not budgeted with a number, and that is a gap rather than a decision: the write lock is held for a whole project (§ 2.5) against the 30 s deadline every `Bulk` writer shares (§ 2.2), so a project slow enough to matter would starve a concurrent export. Nothing measurable exists to state until the loader runs — the honest form is that the implementation measures a full-corpus load and this section records the figure, and the first run that exceeds a few seconds per project is evidence the per-project transaction is too coarse.

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
| `roadmap_migrate_load/` | INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-9, INV-11, INV-12, INV-13, INV-14 |
| `roadmap_store_schema/` | INV-7, INV-8, INV-10 — the store-surface changes, filed with the rest of ANTS-3756's write-path invariants rather than in a second directory |

All **fourteen** invariants are covered; none is a grep-only check. The new directory adds its `test_*.cpp` to **`test_core`**'s `SOURCES` list, per `tests/features/README.md` step 4 and for the ANTS-1217 reason § 4 gives; no `add_executable`.

Per the project convention (`CLAUDE.md`, `testing.md`), each test is verified to **fail against pre-implementation source** before the implementation is restored. Two are worth naming because the mutation is not the obvious one: INV-2's re-run must be shown red against a loader that rewrites every field on every run (which passes a load-once test), and INV-5's must be shown red against in-place position updates (which pass on a plan whose order did not change).

## 7. Cross-doc impact

- **[ANTS-3756](ANTS-3756-roadmap-store-schema.md) is amended, not merely referenced.** § 2.3 changes `putItem()`'s shipped transaction behaviour — including removing its internal `ROLLBACK` when it does not own the transaction — and adds `begin`/`commit`/`rollback`/`inTransaction`; § 2.4 adds eleven more plus an `ItemRead` struct. **Fifteen methods in total land on ANTS-3756's public surface**, and that spec's § 2 and its INV-20 discussion both describe `putItem()` as opening its own transaction, which stops being unconditionally true. The four-argument `setItemField()` is an overload, so its INV-10 is untouched. INV-7, INV-8 and INV-10 above are store invariants and are folded into ANTS-3756's own list at implementation, since a store invariant filed only in a migration spec is one nobody looking at the store will find.
- **[ANTS-3757](ANTS-3757-roadmap-migration-read.md)** — two amendments. § 2.1.1's last two owed rows (`projectId`, `sectionId`) are discharged here; and **§ 2.10's note-code set, which that spec calls closed, gains this half's six load-only codes** (`orphaned_item`, `id_allocated`, `history_capped`, `field_conflict`, `project_refused`, `bad_options`). Both land in its § 7 when this ships — the note codes especially, since `Note::code` is one type shared by both halves and two documents each claiming to hold the whole vocabulary is the drift this bullet exists to stop.
- **[ANTS-3761](ANTS-3761-roadmap-export-format.md)** — the export must round-trip everything this writes. The `element` rows, `section.intro` and `project.legend` are already in its record set; this is the first writer to produce them from anything but a rebuild, so its INV-2 column diff becomes a real test of this half rather than of the rebuild alone.
- **`docs/standards/roadmap-data-model.md`** — § 7.2's allocation policy is executed here and not restated. No amendment: § 2.8 adds mechanics, not policy.
- **`CLAUDE.md`** — the module map gains `src/roadmapmigrateload.{h,cpp}`; `docs/subsystems.md`'s roadmap lane gains the same.
- **`ROADMAP.md`** — ANTS-3765 flips to in-progress at implementation; ANTS-3758 unblocks on ship.
- **`CHANGELOG.md`** — on ship.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 1 | 2026-07-31 | 2 (identical shared packet, cold) | 3 / 5 / 9 / 10 / 2 | 28 verified, **1 dismissed**, all 27 actionable fixed; all draft defects (first gate on this document). **Both lanes independently led on the same three CRITICALs, and all three were the same class: the design named a mechanism the surface it defines cannot perform.** § 2.6's element rebuild had no way to re-file an *existing* item — `addElement()` refuses `kind='item'` and `putItem()` would violate `UNIQUE (project_id, id_fold)` — so every matched item and every orphan ended the transaction unfiled, which is INV-4 and ANTS-3756 INV-20 false at a commit boundary; `fileItem()` added. The re-run's field comparison had nothing to read with (`findItem()` returned only a pk) and its provenance-merge rule was unreachable, because the shipped `setItemField()` hardcodes `asserted`; `readItem()` and a four-argument overload added. And § 2.3 was silent on `putItem()`'s three internal `ROLLBACK` calls — participating unchanged, one bad item would abort the *caller's* transaction from the inside, after which every later write autocommits and persists while `Outcome` reports success: INV-1 inverted with nothing observing it. **One claim was dismissed on verification and it was a CRITICAL in one lane** — both lanes said a re-run collides on the `project` row; `registerProject()` is already get-or-create, selecting on `root` before inserting. The *section* half was real (`addSection()` is a bare INSERT against `UNIQUE (project_id, slug)`), so the finding regraded to HIGH, section-half only. Also fixed: § 2.4's blanket "each canonicalises its JSON" was false for three of four writers and would have had an implementer canonicalise narration prose, which ANTS-3756 § 2.3 calls undefined; INV-3's recipe set `priority` through `setItemField()`, which neither accepts it nor takes an `int`; `history.seq` restarting per run collides on `UNIQUE (item_pk, changed_at, seq)` when two runs share a stamp (now INV-14); § 2.8's prefix derivation was undefined for a project with no ids and for "maximum" over `TEXT`; and an `Outcome::notes` comment said "never a superset" where it meant subset. Doc 267 → 360 lines. |
