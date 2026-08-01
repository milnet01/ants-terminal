# ANTS-3765 — Roadmap migration, load half: atomicity, re-run matching and the cutover interim

**Status:** accepted — cold-eyes loops 1–3 folded, converged by cap (2026-07-31).
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
    // INSERTED-or-UPDATED rows, not attempted ones: `sectionsWritten` counts a
    // section created or whose title/level/intro/parent changed, `elementsWritten`
    // every element row re-inserted by § 2.6's rebuild (so it is non-zero even on
    // an unchanged re-run), `historyRows` the rows § 2.9 appended. INV-13 compares
    // all three between a dry run and the real one.
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

`RoadmapStore` takes its `Access` at construction and ANTS-3756 § 2.5 gives `Access::Bulk` a 30 s busy deadline and a 16 MiB page cache. `load()` therefore does **not** open a connection — it takes an open store, so a caller migrating ten projects opens **one** long-lived `Bulk` connection and calls `load()` ten times. That is the shape RetroDB arrived at after "database is locked" under concurrent bulk jobs. **Both halves of that lesson are load-bearing and an earlier draft set them against each other:** the store's own header attributes the fix to the 30 s deadline (`kBulkBusyTimeoutMs`, whose comment records the RetroDB origin), while the ROADMAP bullet for this id credits the long-lived connection. They are not rivals — a bulk writer needs the longer deadline *and* needs to stop reopening a connection per unit of work — so this spec takes both and claims neither as the sole cause.

It refuses an `Interactive` store rather than proceeding (INV-12). A 5 s deadline against a migration-sized transaction fails *sometimes* — under a concurrent export, on a slow disk — which is the worst available behaviour: it would make ANTS-3757's corpus load pass locally and fail on a loaded machine. This requires `Access` to be readable, which it currently is not; § 2.4 adds the accessor.

### 2.3 The transaction shape, and the one shipped decision this changes

**`putItem()` stops opening its own transaction, and the store gains explicit transaction control.** § 1.1's measurement leaves four candidates and only one survives.

| Option | Verdict |
|---|---|
| Load half writes raw SQL against `db()` | **No.** Makes it the **third** producer of `item` rows, after `putItem()` and ANTS-3761's rebuild. Every future column has three writers to update and INV-21's "canonicalised at the write path" has three paths to be true of. This is the option ANTS-3767 explicitly argued against for the same columns. |
| A per-row transaction flag on `ItemWrite` | **No.** Encodes a property of the *connection* as a property of one *row*. Two calls in one transaction could disagree, and the type system would not care. |
| `QSqlDatabase::transaction()` / `commit()` / `rollback()`, which already exist on the store's `m_db` | **No**, and this is the one an implementer reaches for first, so it is rejected on the record rather than by omission. Qt issues a **deferred** `BEGIN` — checked, not assumed: `strings /usr/lib64/qt6/plugins/sqldrivers/libqsqlite.so \| grep -i '^BEGIN'` returns `BEGIN` alone, with no `IMMEDIATE` form anywhere in the driver; the store's whole concurrency design is `BEGIN IMMEDIATE` (ANTS-3756 § 2.5, INV-16), and a deferred transaction takes its write lock at the first write — so it can fail with `SQLITE_BUSY` *mid-load*, after work has been done, and § 2.8's "two migrations cannot both take ANTS-0042" argument depends on the lock being held from the start. |
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

`putItem()` keeps its current behaviour when no transaction is open — it wraps its item-plus-element write in one, because ANTS-3756 INV-20 (exactly one `kind='item'` element per item) depends on those two inserts being atomic, and every existing caller relies on it. When a transaction *is* open it participates in the caller's, and ANTS-3756 INV-20 holds a fortiori: the enclosing transaction is wider, not narrower.

**Its three internal failure paths stop issuing `ROLLBACK` when they are not the transaction's owner, and this is the sharpest edge in the change.** As shipped, `putItem()` calls `exec(m_db, "ROLLBACK", nullptr)` on each of its three failure paths — the section-project check, the item insert, the element insert. Left unchanged inside an enclosing transaction, one bad item would abort the **caller's** transaction from the inside; `load()` would carry on believing it was still in one, every subsequent write would run in autocommit and **persist**, and `Outcome` would report a clean partial load. That is INV-1 inverted while every count says success — the worst available failure, because nothing observes it. So: when it owns the transaction it rolls back exactly as today; when it does not, it returns `std::nullopt` with the error set and touches the transaction state not at all, leaving the unwind to `load()`. INV-8 asserts both halves.

**This is a change to ANTS-3756's shipped surface and is amended there, not merely described here** (§ 7): a spec that says `putItem()` self-commits while the code takes a caller's transaction is exactly the drift the review gate exists to stop.

The nesting refusal in `begin()` is deliberate and is the one behaviour worth stating twice: a `begin()` that silently no-oped inside an open transaction would make a caller's `commit()` end a transaction it did not start. **SQLite itself refuses the nested `BEGIN`** (§ 1.1's transcript is that refusal, and nothing more — it does not demonstrate a silent no-op, because SQLite does not do one). A no-oping `begin()` would therefore be *this project's* invention, hiding an engine-level refusal behind a caller-owned transaction, which is why the refusal is passed through rather than smoothed over.

**Every other shipped writer `load()` calls is already transaction-free, and that is a precondition of this whole design rather than an assumption.** Verified in shipped source 2026-07-31: `registerProject()`, `addSection()`, `setItemField()` and `appendHistory()` each issue bare queries with no `BEGIN` of their own, so `putItem()` is the only one that needed changing. Were any of the others to self-transact, INV-1 and INV-11 would be unbuildable — so a future writer added to the store must be transaction-free too, or join this section.

### 2.4 What the store owes this half

**Nineteen methods and two structs** — ten writers and nine readers — each traceable to the section that needs it; § 2.3 adds four more, so **twenty-three** methods land on ANTS-3756 in total (§ 7). All are additions rather than corrections, and the readers are as load-bearing as the writers: a re-run cannot decide anything it cannot first read, and the shipped surface has no reader at all beyond `db()`.

**The test of this list is not that it reads complete but that every operation §§ 2.5–2.9 name appears in it.** Two review passes each found methods missing from a list asserted to be exhaustive — the element delete, the section update, the `MAX(seq)` read, the item enumeration — and in each case the prose mandated an operation no declared method could perform. So the traceability is the contract: an operation named in a later section with no method here is a defect in *this* section, and the only sanctioned alternative — raw SQL at the call site — is the one § 2.3's table rejects on the record.

**Implementation found three more of exactly that kind, which is the fourth pass to do so** (2026-08-01), and they are marked below rather than folded in silently: `readSection()` — § 2.6 gives items `readItem()` for its "written only if it differs" comparison and gave sections nothing, so `Outcome::sectionsWritten`, declared as counting the sections that *changed*, was not computable; `idPrefixFor()` — § 2.8 step 1 makes the project's existing `id_prefix` row win, and `idHighWater()` takes the prefix as an argument, so that branch could never be reached; and `unfileItem()` — § 2.6 step 3 files *every* matched item through `fileItem()`, which refuses an already-filed one, and step 2 clears only the plan's sections, so an item still in source whose stored section the plan no longer carries (a heading deleted while its bullets moved elsewhere) kept its old element row and aborted the project. Clearing that retained section instead is what § 2.6 step 2 forbids, so the delete has to be per item. The recurrence is itself the finding: four passes over one list, each asserting it complete, each wrong — which is why the traceability rule above replaced the adjective.

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
// and putItem()/fileItem() stay the only ways an item is filed
// (INV-10 below, ANTS-3756 INV-20).
// payload is canonicalised when kind='table' and stored VERBATIM when
// kind='narration' (ANTS-3756 § 2.3).
bool addElement(qint64 sectionId, int position, const QString &kind,
                const QString &payload, QString *error = nullptr);

// The re-filing writer § 2.6's element rebuild needs, and the piece whose
// absence made that rebuild unbuildable: putItem() files a NEW item, and an
// item that already exists cannot be re-inserted (UNIQUE (project_id, id_fold)).
// Writes one kind='item' element row for an item that currently has none.
// Refuses if the item is already filed — ANTS-3756 INV-20's "at most one" is
// the partial index, and this must not be the way round it.
bool fileItem(qint64 itemPk, qint64 sectionId, int position, QString *error = nullptr);

// ADDED AT IMPLEMENTATION. The inverse of fileItem(), for the one case § 2.6
// step 3 cannot otherwise reach: a MATCHED item whose stored section the plan
// no longer carries. Step 2 clears only the plan's sections, so that item's
// element row survives, fileItem() refuses it, and § 2.5 turns the refusal
// into a rolled-back project — on an edit as ordinary as deleting a heading
// and moving its bullets. Per ITEM and not per section, for step 2's reason.
bool unfileItem(qint64 itemPk, QString *error = nullptr);

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

// § 2.6 rule "empty does not overwrite"'s one exception needs to write SQL
// NULL, and setItemField() binds a QString — so `''` and NULL are the same
// call. A cleared field is therefore its own method rather than a convention
// about empty strings.
bool clearItemField(qint64 itemPk, const QString &field, const QString &provenance,
                    QString *error = nullptr);

// § 2.6 step 2's element delete, PER SECTION and never per project. The
// signature is the contract here: a project-wide delete would take out the
// narration and table rows of sections the plan no longer carries, which
// nothing re-inserts, and § 2.1 makes the declaration win over the prose — so
// a method named for the project would mandate exactly the permanent loss
// § 2.6 forbids, whatever the paragraph says.
bool clearSectionElements(qint64 sectionId, QString *error = nullptr);

// § 2.6's section update. addSection() is INSERT-only, and a re-run can change
// a heading's title, its level, or its parent. Takes the whole tuple rather
// than one field per call: they come from one PlannedSection and a partial
// update has no meaning.
bool updateSection(qint64 sectionId, const QString &title, int level,
                   std::optional<qint64> parentId, QString *error = nullptr);

// --- readers ---

// Re-run matching (§ 2.6) — resolve an id within one project, folded.
// The error out-param is not decoration: without it `nullopt` conflates "no
// such item" with "the query failed", and the failure path of that confusion
// is an INSERT of an item that already exists.
std::optional<qint64> findItem(qint64 projectId, const QString &id,
                               QString *error = nullptr) const;

// § 2.6's field comparison. Returns the stored item in the SAME type the writer
// takes, so "compare, then write the difference" needs no second shape — and
// `ItemWrite` already carries `sectionId`/`position`, which is the current
// filing § 2.7 must capture BEFORE the element rebuild deletes it. An earlier
// draft wrapped this in an `ItemRead` carrying its own copy of both; two
// sources for one fact is the defect this spec rejects elsewhere (§ 2.10).
std::optional<ItemWrite> readItem(qint64 itemPk, QString *error = nullptr) const;

// One enumeration serving BOTH § 2.7's orphan detection and § 2.6.1's id-less
// re-run matching. `findItem()` is a point lookup; orphan detection is a set
// complement and id-less matching is a search by natural key, and neither is
// expressible as a point lookup however many times it is called.
//
// `idFold` and not `id`: matching is case-folded within a project (ANTS-3756
// INV-3), so a set difference over raw ids reports a stored `SH-1` as an
// orphan of a planned `Sh-1`.
// `idFromMigration` is `provenance.id == "migrated"` — the marker ANTS-3757
// § 2.9 already requires — and it is what makes § 2.6.1 safe: only rows this
// migration gave an id to are eligible for a headline re-match.
struct ItemRef {
    qint64  itemPk;
    QString idFold, headline;
    qint64  sectionId;
    bool    idFromMigration;
};
std::optional<QVector<ItemRef>> listItems(qint64 projectId,
                                          QString *error = nullptr) const;

// Section resolution on a re-run. addSection() is a bare INSERT and collides on
// UNIQUE (project_id, slug) the second time, so a re-run MUST resolve first.
// registerProject() needs no equivalent: it is already get-or-create, selecting
// on `root` before inserting (verified in shipped source, 2026-07-31).
std::optional<qint64> findSection(qint64 projectId, const QString &slug,
                                  QString *error = nullptr) const;

// ADDED AT IMPLEMENTATION — the section half of § 2.6's "written only if it
// differs". `findSection()` resolves a slug to an id and says nothing about
// what the row holds, so `Outcome::sectionsWritten` could not have counted
// what it is declared to count.
struct SectionRow {
    QString slug, title, intro;
    int     level;
    std::optional<qint64> parentId;
};
std::optional<SectionRow> readSection(qint64 sectionId, QString *error = nullptr) const;

// ADDED AT IMPLEMENTATION — § 2.8 step 1's first branch. `idHighWater()` takes
// the prefix as an argument, so "the project's existing id_prefix row when it
// has one" was unreachable through the declared surface, and a renamed project
// directory would have silently started a second counter. nullopt ⇒ no row.
std::optional<QString> idPrefixFor(qint64 projectId, QString *error = nullptr) const;

// § 2.8's starting point. Absent row ⇒ nullopt, which is not an error.
std::optional<qint64> idHighWater(qint64 projectId, const QString &prefix,
                                  QString *error = nullptr) const;

// § 2.9's seq continuation. appendHistory() takes `seq` from the caller, so
// the caller needs the current maximum. Absent rows ⇒ nullopt, not an error.
std::optional<int> maxHistorySeq(qint64 itemPk, const QString &changedAt,
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

#### 2.6.1 The id-less case, which is most of the corpus

**An item with no id in source cannot be matched by id, and it is not a rare shape: ANTS-3757 § 2.9 counts ~1,600 id-less items across the ten projects.** § 2.8 allocates their ids *inside the store* and § 5 forbids writing anything back to the source file, so the second run sees the same bullets id-less again. Matched by id alone, every one of them is inserted afresh with a *newly allocated* id while the first run's row becomes an orphan — the corpus duplicated on every re-run, ids burnt at ~1,600 per pass, and INV-2 false for almost every real plan. So id-less items get their own rule:

> An id-less plan item matches a stored item when that item is filed **in the same section**, its `provenance.id` is **`migrated`** (so migration, not an author, gave it its id), and its **headline is byte-identical**. `listItems()` supplies all three. No match ⇒ a new item, and § 2.8 allocates.

**This is a weaker key than an id and the weakness is stated rather than hidden.** § 2.6 refuses to match id-bearing items on headline because a headline changes while an item stays itself — and that is exactly what this rule tolerates, because for an id-less item there is nothing stronger available: editing such a bullet's headline *does* orphan it and create a new item. Two guards keep the damage bounded. The `provenance.id == "migrated"` test means an author's own id-bearing item is never re-matched this way. And **an ambiguous match — two stored items, same section, same headline, both migration-allocated — is never guessed**: the plan item is treated as new and an `ambiguous_rematch` note names it, because picking one of two would silently move history onto the wrong item.

**The durable fix is out of this half's scope, and naming it is part of the contract.** Writing allocated ids back into `ROADMAP.md` would make every later run match by id and retire this rule entirely; that is a write to the *source*, which § 5 excludes and ANTS-3758 owns. Until then this rule is what keeps a re-run idempotent.

**The project and its sections are resolved before anything is written.** `registerProject()` is already get-or-create — it selects on `root` and returns the existing `project_id` — so it needs no change and a re-run reuses the same project row. `addSection()` is **not**: it is a bare INSERT and collides on `UNIQUE (project_id, slug)` the second time, which would abort every re-run at its first section. So a re-run resolves each section with `findSection()` and calls `addSection()` only for a slug that is genuinely new, updating `intro`, `title`, `level` and `parent_id` on the ones that exist, parents before children so a `parentSlug` always resolves to a row that exists. **A section in the store and absent from the plan is retained, not deleted** — for § 2.7's reason, one level up: it may hold an orphaned item, and deleting it would orphan that item's filing rather than its row.

For a matched item, each field the plan carries is compared with what `readItem()` returns and written only if it differs (`Outcome::itemsUpdated` versus `itemsUnchanged`). Four rules make that comparison well-defined, and § 2.11 cites them by **name** rather than by ordinal, because a list that gains a rule renumbers every citation to it:

- **The plan is authoritative only for the fields it carries.** ANTS-3757 § 2.1.1 lists which those are, and `priority`, `resolution`, `milestone`, `visibility` and the dates are not among them — a source file cannot express them. A re-run therefore never clears a field a human set through `roadmap_log`, which is the single most destructive thing a re-run could do.
- **`provenance` is merged, not replaced.** A field this run did not write keeps the provenance it had; a field it wrote takes the plan's (`asserted`/`defaulted`/`migrated` per ANTS-3757 § 2.7–2.9). This is reachable only through § 2.4's four-argument `setItemField()`: the shipped three-argument form hardcodes `asserted`, which is correct for a human edit and wrong for every write migration makes.
- **Rule "empty does not overwrite":** an empty plan value does not overwrite a non-empty stored value — with one exception, `body`, which is the only field whose emptiness in source is meaningful (a bullet whose continuation lines were deleted). The asymmetry is stated because a reader will otherwise read the general rule as covering it. Every suppressed overwrite raises a `field_conflict` note, which is that code's only trigger. **A cleared `body` is written as SQL `NULL`, not `''`, via `clearItemField()`** — `putItem()` binds an empty `body` as NULL while `setItemField()` binds the `QString` it is given, so `''` and NULL are the same call there and the clear needs its own method. Without the rule the same logical state has two representations depending on which path last touched it, and ANTS-3761's INV-2 column diff would see a difference where there is none.
- **`id_origin` is fixed at insert and never updated.** The plan carries it, and a re-run can in principle see it change (a quarantined id later given a real one), but the store's identity is `(project_id, id_fold)`: an id that changed is a *different* item by the store's own rule, matched as a new insert plus an orphan rather than as an update. So there is nothing for an `id_origin` update to mean, and `setItemField()`'s allowlist rightly excludes it.

**Ordering is rebuilt, not diffed.** The `element` table has `UNIQUE (section_id, position)`, and SQLite enforces it **per row, as each row is written** — so an in-place shift fails at the first row whose new position is still held by a row that has not moved yet. Inside the transaction, `load()` therefore:

1. **captures each existing item's filing first** — `readItem()` returns `sectionId` and `position`, and once the element rows are gone the store has no other record of where an item was filed (ANTS-3756 INV-20: "there is no `item.section` column");
2. calls `clearSectionElements()` once **per section the plan carries** — never a project-wide delete. The distinction is load-bearing: a section retained because the plan no longer mentions it (below) also holds narration and table rows the plan cannot supply, so a project-wide delete would destroy payload no later step re-inserts, silently and permanently;
3. re-inserts from the plan, in one pass per section covering **all three** element kinds — new items via `putItem()`, which files as it inserts; already-existing items via `fileItem()`, the writer § 2.4 adds for exactly this and without which every matched item would end the transaction unfiled; and the plan's `narration` and `table` elements via `addElement()`;
4. re-files **every orphan whose element row step 2 deleted** — that is, orphans in sections the plan still carries — before commit. Orphans in retained sections are untouched: their element row was never deleted, and `fileItem()` refuses an already-filed item (§ 2.4), which § 2.5 would turn into a rolled-back project. Both kinds count toward `itemsOrphaned`.

Items survive the delete — removing an element row does not touch the `item` it references — so INV-20 holds at every commit boundary even though it is transiently false inside the transaction. That is a property of the transaction, not a loophole: nothing outside it can observe the intermediate state.

### 2.7 An item in the store and absent from source

**It is never deleted, and it is never silently kept.** Detection is `listItems()`'s set minus the plan's ids, **compared on `id_fold`** — a raw-id difference would report a stored `SH-1` as an orphan of a planned `Sh-1`, since matching is case-folded within a project (ANTS-3756 INV-3). The item retains its row, its history and its identity; its status is left exactly as it was, and it is counted in `itemsOrphaned` and reported with the `orphaned_item` note code naming its id.

**Where it is re-filed needs stating, because § 2.6 deletes the only record of where it was.** Its `(sectionId, position)` is captured by `readItem()` before the delete, and it is re-filed **at the end of that same section — after every *element* the plan placed there**, not merely after every item. `position` is one sequence per section shared by items, narration and tables alike (ANTS-3757 § 2.11), so "after the last item" can land on a position a narration row already holds and fail `UNIQUE (section_id, position)`.

Two cases the first draft left to the implementer:

- **Several orphans in one section** are appended in ascending order of their captured `position`, ties broken by `item_pk`. Any total order would satisfy INV-4; an unstated one makes the re-run non-deterministic, which ANTS-3761's export order and § 2.6's "rebuilt, not diffed" claim both depend on.
- **The orphan's section is gone from the store entirely.** Nothing in *this* spec produces that state — § 2.6 retains every section and § 5 forbids deletion — so the reachable producers are outside it: a store rebuilt by ANTS-3761 from an export that omitted the section, or a hand-edited store. It is handled rather than assumed away because the loader cannot tell which tool wrote the store it opened. The orphan is filed under the synthetic root section (empty slug and title, level 0), **created on demand if absent**, which is the same row ANTS-3757 § 2.1 gives the plan for content above the first heading. A section merely absent from the *plan* is a different case: that row survives (§ 2.6) and the orphan stays where it was.

Deleting would be wrong in both directions. An id absent from source is far more often a **rename or an archive rotation** (`roadmap-format.md` § 3.9 moves closed minors out of `ROADMAP.md`, which ANTS-3757 § 5 excludes from the plan, so *every archived item looks deleted to this half*) than a real deletion — and the store is primary (ANTS-3756 § 1), so it holds `history` rows and relationships that the source file never contained and cannot restore. Marking it `dropped` instead would be no better: `dropped` is an author's statement about the work, and inferring it from a file edit puts a claim in the store that no author made.

The consequence is stated rather than hidden: after the archive rotation ANTS-3766 addresses, a re-run reports every archived item as orphaned. That is noise, not damage, and ANTS-3766 removes it at the source.

### 2.8 Id allocation

ANTS-3757 § 2.9 leaves the obligation on the item — `PlannedItem::idAllocationOwed` and `closed` — because a pure planner cannot hold a counter. `load()` allocates **inside the same transaction as the write**, which is what makes the counter safe: two migrations racing on one store cannot both take `ANTS-0042`, because the second blocks on `BEGIN IMMEDIATE`.

The policy is `roadmap-data-model.md` § 7.2's and is not restated here. What this half adds is the mechanics, and each step needs a rule rather than an intuition — "the maximum id" is not defined over `TEXT`:

1. **The prefix, and what a prefix even is.** An id's prefix is **the text before its final `-`**; an id with no `-` (`Sh4`, the legacy bold form) contributes none. That rule has to be stated because it is not the only candidate — over `ANTS-3765`, `Ts20-SP6` and `Sh4`, "before the first `-`", "before the last `-`" and "the leading alphabetic run" give three different answers. Then: the project's existing `id_prefix` row when it has one; failing that, the **most frequent** prefix among the plan's `idOrigin == "parsed"` ids, ties broken by first appearance in document order; failing *that* — a project with no id-bearing items at all, the first-run case for several of the corpus's ten — the same fallback `roadmap_log` uses, **the uppercased first four characters of the project root's leaf directory**. (An earlier draft said "the caller's `exportSlug` uppercased, which is how `roadmap_log` derives one today". That was wrong on both halves: the verb's contract derives it from `caller_cwd`'s leaf directory, and `export_slug` is `[a-z0-9-]`-constrained, so uppercasing one can yield a hyphen-bearing prefix that step 2's suffix rule would then mis-split.)
2. **The starting high-water.** `idHighWater()` when the row exists. Otherwise the maximum **numeric suffix** among the plan's parsed ids carrying the chosen prefix — the digits after the final `-`, ignoring any id whose suffix does not parse as an integer. Quarantined and synthesised (`PASS-N-M`) ids are excluded from both steps: neither comes from the counter, and a `PASS-43-5` would otherwise set the high-water to 5.
3. Each allocation increments it; `raiseIdHighWater()` writes the final value before commit. **The rendered text is `<prefix>-<suffix zero-padded to four digits>`** — `ANTS-0042`, and `ANTS-12345` once the counter passes four digits, matching `roadmap-format.md` § 3.5.1's own form. Left unstated an implementer invents a width, and the choice is permanent: it is baked into every stored id and every citation of it thereafter.
4. When the plan carries **no** parsed id yielding an integer suffix — a project whose ids are all `Ts20-SP6`-shaped, or which has none at all — the high-water starts at **0**, so the first allocation is `-0001`.

`provenance.id` is `migrated` for every allocated id, per ANTS-3757 § 2.9.

**`id_origin` for an allocated id is `synthesised`, and implementation had to decide it because nothing stated it.** ANTS-3757 leaves `PlannedItem::idOrigin` **empty** for an id-less item — verified in `src/roadmapmigrate.cpp`, whose id-less branch sets `idAllocationOwed` and `provenance.id` and no origin — while `item.id_origin` CHECKs `('parsed','synthesised','quarantined')`, so the load must supply one of the three. `parsed` would be a lie and `quarantined` a worse one; `synthesised` is the only one that is true of an id nothing in the source file contained. It carries no load-bearing weight either way, because the fact § 2.6.1's re-match keys on is `provenance.id == "migrated"`, not this column.

**A rolled-back load allocates nothing**, which is the whole reason allocation is inside the transaction rather than before it: an id burnt by a failed run is an id that exists in no document and blocks a future one.

### 2.9 History

The **initial** load of an item writes no `history` rows — there is no prior value to record, and manufacturing one would put a change in the audit trail that never happened. A **re-run update** writes one row per changed field, with `changed_at` from `Options::changedAt`, so the store can say what a source edit changed and when it was picked up.

**`seq` continues from the stored maximum for that `(item_pk, changed_at)`, and does not restart at zero.** `history` carries `UNIQUE (item_pk, changed_at, seq)`, and `changedAt` is one stamp per migration rather than per row — so two runs given the same stamp (a scripted re-run, a test, a caller that stamps once a day) collide on the second run's first row and abort the whole project. `maxHistorySeq()` supplies the starting point — the next row is its value **plus one** — `appendHistory()` takes `seq` from its caller, so without that reader the rule is unimplementable through the declared surface. **The first row for a given `(item_pk, changed_at)` is `seq = 0`**, and each subsequent row in the same run increments; a `nullopt` from `maxHistorySeq()` therefore means 0, not 1. Continuing from the stored maximum costs one query per updated item and removes a failure whose trigger is a caller doing something entirely reasonable.

`appendHistory()` already fails-and-reports at the store-wide cap while the item write it accompanies succeeds (ANTS-3756 INV-14). This half does **not** override that: a history write that fails at the cap does not roll the project back.

**How the loader tells that failure from any other one has to be stated, because the method reports both the same way** — `bool` plus an error string, and string-matching an error message is the invention this sentence exists to prevent. After a failed `appendHistory()` the loader **re-evaluates the store's own predicate**: the write was refused at the cap exactly when `historyBytes() + (field + oldValue + newValue).size() > historyCapBytes()`. Both terms are public and all three strings are in the loader's hand, so the test is reproducible exactly, with no error text parsed. True ⇒ the § 2.5 exception and the project continues; false ⇒ the write failed for some other reason and the project aborts.

**An earlier form of this rule compared `historyBytes()` against `historyCapBytes()` alone, and implementation disproved it** (2026-08-01). `appendHistory()` refuses when *stored plus incoming* exceeds the cap, so at the moment of refusal the stored bytes are still **below** it in every case but an exact landing — most sharply on the first refusal against an empty `history`, which compares 0 against the cap and reads as "some other failure". The rule would therefore have aborted precisely the project this exception exists to save, and INV-15 is what surfaced it: its 8-byte injected cap makes the first re-run update the refused one. Losing the migration of a project because its audit trail is full inverts the priority — the item is the data, the history is the record of it — and the failure is reported through `Outcome::notes` rather than swallowed.

### 2.10 The cutover interim

Some projects are migrated and others are not, for as long as the rollout takes, and **`project.root` is the marker** — a project row exists exactly when that project has been loaded, and it carries the canonical root that ANTS-3756 INV-8 keys on. No schema change, no `migrated_at` column, no `PRAGMA user_version` bump: the fact is already recorded by the data.

That works because per-project atomicity (§ 2.5) makes "half a project" unreachable. A project row that exists is a project whose whole plan committed, so the marker cannot be observed in a partial state — which is exactly the property a `migrated_at` column would have had to be maintained by hand to provide.

What consumes the marker is ANTS-3758, not this half: a read verb asks whether the caller's project has a row and falls back to `roadmap_query`'s markdown path when it does not. This spec states the marker and its guarantee; it does not build the fallback.

### 2.11 The report

`Outcome` is a value and every count in it is assertable, for the reason ANTS-3757 § 2.10 gives: a migration that reports to stderr is a migration whose behaviour no test can pin. The plan's own notes are carried through unchanged and joined by the codes only a load can raise.

**These seven codes EXTEND ANTS-3757 § 2.10's set, which that spec calls closed.** Naming them here without saying so would leave two documents each claiming to hold the whole vocabulary — § 7 records the amendment.

**`Note::line` is `0` on every one of them**, which that type defines as "whole-file". A plan note is keyed to the source line it came from; a load note is about a *store row*, and the load never opens the source file (§ 5), so it has no line to report. Carrying a plausible-looking line would be worse than carrying none.

| Code | Raised when |
|---|---|
| `orphaned_item` | § 2.7 — in the store, absent from source. Detail names the id. |
| `id_allocated` | § 2.8 — an id-less item was given one. Detail names the new id. |
| `history_capped` | § 2.9 — `appendHistory()` refused at the store-wide cap. |
| `field_conflict` | § 2.6 rule 3 — the plan carries an **empty** value for a field whose stored value is non-empty, so the empty is not written. Detail names the id and the field. (An earlier draft defined this as a disagreement over a field the plan is *not* authoritative for, which has no reachable trigger: a field the plan does not carry has no value to disagree with. This is the case that actually occurs, and rule 3 is where it is decided.) |
| `project_refused` | § 2.5 — the whole project rolled back. Detail is `Outcome::error`. |
| `bad_options` | § 2.1 — `Options::changedAt` is absent or not an ISO-8601 Z stamp. Raised before the transaction opens, so nothing is written. |
| `ambiguous_rematch` | § 2.6.1 — two stored items in one section share a headline and were both migration-allocated, so an id-less plan item cannot be re-matched to either. Treated as new; detail names the headline. |

## 3. Invariants

- **INV-1** — **One project is one transaction.** A `load()` that fails anywhere except § 2.5's one stated exception leaves **no row in any table this load would have written**: no project row, no sections, no items, no elements, no id-prefix advance. (Byte-identity of the file is deliberately *not* claimed — WAL frames and freelist pages move under a rolled-back transaction, so it would be unassertable.) *Test:* `roadmap_migrate_load` builds a plan whose Nth item violates a CHECK — an off-enum `status` — and asserts every table is empty and `ok == false`. **The test constructs the `MigrationPlan` struct directly rather than parsing markdown**, which is what makes that fault injectable: ANTS-3757 § 2.7's status vocabulary is total, so no real source file can produce an off-enum status, and a fixture-driven test could not build this case at all. *Breaks when:* the writes run outside a transaction — the shipped `putItem()` shape, which commits each item as it goes and leaves N−1 rows behind.
- **INV-2** — **A re-run over an unchanged source changes no item and writes no history.** (Not "changes nothing": § 2.6 rebuilds the element rows unconditionally, so `element_id` rowids do move. The claim is about items and the audit trail, which is what a caller can observe.) Loading the same plan twice produces `itemsInserted == 0` and `itemsUpdated == 0` on the second run, and no new `history` rows. *Test:* `roadmap_migrate_load` runs `load()` twice with one `Options::changedAt`, asserting the counts and `SELECT COUNT(*) FROM history == 0`. **Two legs, and the second is the one that matters**, because the corpus is mostly id-less (§ 2.6.1): (a) a plan whose items all carry ids; (b) **a plan whose items carry none**, where the second run must still report `itemsInserted == 0`, `itemsOrphaned == 0` and `idsAllocated == 0`. Leg (a) passes against a loader with no id-less rule at all, which is why it cannot be the only leg. *Breaks when:* matching is on anything but `(project_id, id_fold)` for id-bearing items or § 2.6.1's natural key for id-less ones; or a field comparison treats "absent from plan" as "set to empty" — each rewrites every item on every run and fills `history` with changes that did not happen. Leg (b) additionally breaks whenever the id-less rule is missing entirely: every id-less item is re-inserted with a fresh id and its predecessor orphaned, so `itemsInserted` equals the plan's size on a run that changed nothing.
- **INV-3** — **A re-run never clears a field the plan does not carry.** An item whose `milestone`, `resolution`, `visibility` or `priority` was set through the store keeps it across a re-run that does not mention it. *Test:* `roadmap_migrate_load` loads, sets **`milestone`** via `setItemField()`, re-loads, asserts it survives. The field choice is load-bearing: `priority` is in neither `setItemField()`'s allowlist nor `QString`-typed, so the obvious recipe — set `priority`, re-load — cannot run at all, and `milestone` is the nearest field that is both writable and outside the plan's set. *Breaks when:* the update path writes a full `ItemWrite` rather than the differing fields — the obvious implementation, and the one that silently destroys human edits.
- **INV-4** — **An item absent from source is retained, re-filed and reported.** Its row, its `history` and its status survive; it has exactly one `element` row after the re-run; `itemsOrphaned` counts it and an `orphaned_item` note names it. *Test:* `roadmap_migrate_load` loads two items, **re-loads with the second item's headline changed so it acquires a `history` row**, then re-loads a plan carrying only the first — and asserts the second's row *and that history row* survive, that both items still satisfy INV-20, and that `itemsOrphaned == 1`. The intervening run is what makes the history half testable: § 2.9 writes no history on an initial load, so a load-then-omit recipe asserts the survival of rows that were never created. *Breaks when:* the element rebuild (§ 2.6) re-inserts only the plan's items, which leaves the orphan unfiled and INV-20 false at a commit boundary.
- **INV-5** — **Ordering is rebuilt without a UNIQUE collision.** Re-loading a plan whose items are in a different order succeeds, and afterwards each section's positions are exactly `0..n-1` with no gaps and no duplicates. *Test:* `roadmap_migrate_load` loads a three-item section, re-loads it reversed, asserts the order and that `SELECT COUNT(*) FROM element` is unchanged. *Breaks when:* positions are updated in place — `UNIQUE (section_id, position)` fires against a row that has not moved yet.
- **INV-6** — **A rolled-back load allocates no id.** After a failing load of a plan containing id-less items, `id_prefix` holds **no row** for that project on a first run, and **exactly its pre-run `high_water`** on a re-run. *Test:* `roadmap_migrate_load`, two legs asserting those two values — the first-run leg asserts `SELECT COUNT(*) FROM id_prefix == 0`, the re-run leg seeds a high-water of 41, fails a load that would have allocated, and asserts it is still 41. Stating the expected value in both branches is the point: "asserts the high-water" is satisfiable by a test that asserts nothing. *Breaks when:* allocation runs before the transaction, which burns ids that appear in no document.
- **INV-7** — **`begin()` refuses to nest.** Calling it inside an open transaction fails and reports rather than no-oping. *Test:* `roadmap_store_schema` asserts the second `begin()` returns false and that a subsequent `commit()` still commits the first one's writes. *Breaks when:* it no-ops, so a caller's `commit()` ends a transaction it did not open. (§ 1.1's transcript shows SQLite *refusing* the nested `BEGIN`; a no-op would be this project's own invention layered over that refusal, not something the engine does.)
- **INV-8** — **`putItem()` is atomic with or without an enclosing transaction, and never rolls back one it does not own.** With no transaction open it self-commits; with one open it participates, writes nothing durable until the caller commits, and on failure returns `std::nullopt` **without issuing `ROLLBACK`**. *Test:* `roadmap_store_schema`, three legs — the existing standalone case; a `putItem()` inside a rolled-back transaction leaving no item **and** no element row; and a *failing* `putItem()` inside an open transaction after which `inTransaction()` is still true and a subsequent write is still rolled back by the caller. The third leg is the one that matters: without it, a `putItem()` that aborts the caller's transaction from the inside passes the first two while every later write silently autocommits. *Breaks when:* the shipped `ROLLBACK` calls are left on the failure paths, or the transaction check reads the connection rather than the store's own flag.
- **INV-9** — **The two JSON-writing methods § 2.4 adds store canonical JSON, and the other two store their text verbatim.** `setLegend()` and `addElement()` with `kind='table'` produce the same bytes ANTS-3761's export would; `setSectionIntro()` and `addElement()` with `kind='narration'` store prose **unchanged**. *Test:* `roadmap_store_schema` writes an out-of-order legend object and a table payload and asserts the stored text is sorted and compact, then writes a narration payload containing `{"b":1,"a":2}` as literal prose and asserts it round-trips byte-for-byte. *Breaks when:* either JSON writer binds `QJsonDocument::toJson(Compact)` — the ANTS-3767 failure, one column further along — or the blanket reading is taken and narration prose is canonicalised, which ANTS-3756 § 2.3 calls undefined rather than merely wasteful.
- **INV-10** — **`addElement()` cannot file an item, and `fileItem()` cannot double-file one.** `addElement()` refuses `kind = 'item'` outright (it has no `item_pk` parameter, so it could not produce a well-formed filing even if it tried); `fileItem()` refuses an item that already has an element row. Together they keep `putItem()` and `fileItem()` the only paths by which an item acquires a filing. *Test:* `roadmap_store_schema` asserts both refusals. *Breaks when:* `addElement()` passes `kind` through to the INSERT and lets the DDL CHECK decide, or `fileItem()` omits its already-filed check and leans on `elem_item_uq` — which does catch it, as a constraint violation that aborts the caller's whole migration rather than a reported refusal.
- **INV-11** — **A project row exists exactly when that project's plan committed.** After a failed load there is no row; after a successful one there is exactly one, keyed on the canonical root. *Test:* `roadmap_migrate_load` asserts both directions. *Breaks when:* the project row is written before the transaction, or outside it — which makes the § 2.10 cutover marker claim a migration that did not happen.
- **INV-12** — **A load against an `Interactive` store is refused.** *Test:* `roadmap_migrate_load` constructs an `Interactive` store and asserts `ok == false` with nothing written. *Breaks when:* the profile is not checked — the load then works on a quiet machine and fails at 5 s against a concurrent export, which is worse than failing always.
- **INV-13** — **`dryRun` writes nothing and reports what a real run would have done.** Every count of a dry run equals the same count of the real run that follows it, and the store is unchanged after the dry run. *Test:* `roadmap_migrate_load` compares the two `Outcome`s **count by count, excluding `projectId`** — that is a rowid the dry run rolled back, so any equality between the two is incidental and asserting it would pass or fail on SQLite's rowid reuse rather than on this invariant. *Breaks when:* the dry run short-circuits before the constraint-bearing writes, which makes it a syntax check wearing an atomicity check's clothes.
- **INV-14** — **A re-run's `history` rows never collide.** Two loads given the **same** `Options::changedAt`, both updating the same field of the same item, produce two `history` rows rather than a constraint violation. *Test:* `roadmap_migrate_load` loads, re-loads with a changed headline and one stamp, re-loads again with a second changed headline and **the same** stamp, and asserts two history rows and `ok == true` throughout. *Breaks when:* `seq` restarts at 0 per run — `UNIQUE (item_pk, changed_at, seq)` then aborts the third load, and the trigger is a caller stamping two runs identically, which is not a misuse.
- **INV-15** — **A `history` write refused at the cap does not abort the project.** The item update it accompanies commits, `ok` is `true`, and a `history_capped` note carries the loss. This is the *only* exception to INV-1, and it is asserted rather than merely written down, because an exception no test covers is one an implementer may reasonably read as an error path. *Test:* `roadmap_migrate_load` with an injected `historyCapBytes` small enough that the first re-run update crosses it (the same injection ANTS-3756 INV-14 uses), asserting the item's new headline is committed, `ok == true`, and the note is present. *Breaks when:* the loader treats every `appendHistory()` failure alike and rolls back — losing a project's whole migration because its audit trail is full, which inverts the priority between the data and the record of it.

## 4. RAM / build cost

**Memory.** One plan at a time, and ANTS-3757 § 4 already budgets the plan itself; this half adds the `Bulk` connection's **16 MiB** page cache (ANTS-3756 § 2.5, `kBulkCacheKiB`) plus one `readItem()` result at a time. It deliberately does **not** build an in-memory map of the project's existing items: the lookup is an indexed point query on `(project_id, id_fold)`, and a map would trade a bounded cost for one that grows with the corpus. The per-project item counts are ANTS-3757 § 1.1's and are not restated here — that section is the single home for every corpus figure precisely because the same count drifting between documents was a finding in two of its review loops. Ten projects migrated through one connection cost one 16 MiB cache, not ten, which is the second reason § 2.2 takes an open store rather than opening its own.

**Time — ceiling 1 s per project**, against the 30 s deadline every `Bulk` writer shares (§ 2.2). The write lock is held for a whole project (§ 2.5), so a project slower than that is starving a concurrent export, and the per-project transaction is then too coarse — that is the trigger, and it is a number rather than "a few seconds" so that it can actually fire.

**This replaces a provisional 5 s, and the replacement is measured** (2026-08-01, quiet machine — no build or test process running, two runs agreeing to within 2 ms). A throwaway driver ran `findRoadmap()` + `planFrom()` + `load()` over the ten real project roots under `/mnt/Games/Scripts/Linux/`, into one `Bulk` connection on a `QTemporaryDir` store, then loaded each a second time:

| | Initial load | Re-run |
|---|---|---|
| Worst project that completes (Music_Production, 357 items / 455 elements) | **24 ms** | 26 ms |
| All ten projects, one connection | **200 ms** | — |

Two things the run establishes beyond the figure. **A re-run costs what an initial load costs** — 26 ms against 24 — so the extra reads § 2.6 pays for matching do not change the order of the cost, and INV-2 held on all seven loadable projects: every one reported `itemsInserted == 0`, `itemsUpdated == 0`, `itemsOrphaned == 0`, `idsAllocated == 0` and every item unchanged, against a corpus that is ~40% id-less and therefore matched through § 2.6.1's natural key rather than by id.

**Three of the ten do not complete, and that is ANTS-3757 § 2.5's designed behaviour rather than a defect here.** 3D_Engine, RetroDB and Ants_Terminal carry ids that fold to the same value — 17, 2 and 7 `duplicate_id` notes respectively — and that section states plainly that such a source "fails at ANTS-3765's insert, in the half that cannot see the source line that caused it", which is why it reports the collision where the lines are still visible. The project is refused whole (§ 2.5), the other seven migrate, and the plan's own notes name the lines to fix. Their timings are therefore lower bounds, not loads: Ants_Terminal — the largest roadmap in the corpus at 2.9 MB — reaches its first collision at 110 ms. At the measured 67 µs per item, its ~2,300 items extrapolate to ~160 ms, so **no project in the corpus is within an order of magnitude of the ceiling**, which is the headroom the figure is chosen to leave.

**Build.** Two new files in an existing library, no new target, no new dependency — `ants_roadmapstore_lib` already links `Qt6::Sql`. The feature test joins `test_core`'s `SOURCES` per `tests/features/README.md`; ANTS-1217 consolidated 141 standalone binaries into seven bundles to bound build-time RAM and a new binary would reverse it.

## 5. Out of scope

- **Reading, parsing and planning** — [ANTS-3757](ANTS-3757-roadmap-migration-read.md). This half never opens a `ROADMAP.md`.
- **The read verbs, the published render, and the fate of `roadmap_query` / `roadmap_log` / `RoadmapDialog`** — ANTS-3758. § 2.10 defines the cutover marker; consuming it is that id's.
- **Rotated archives** — ANTS-3766, inherited from ANTS-3757 § 5. § 2.7 states the consequence for orphan reporting in the meantime.
- **Deleting anything from the store.** Not deferred — § 2.7 decides against it, and a later id that wants deletion is proposing a policy this spec argues with rather than completing work it left.
- **A CLI or MCP verb that drives the migration.** This is a library surface; who calls it, and whether a user can trigger it, is ANTS-3758's.
- **Migrating the store's own schema.** `user_version` stays 1: the store has never shipped data, so there is nothing to migrate from.
- **`relationship`, `citation` and `feedback_ref` — written by nothing here, and that is the plan's shape rather than an oversight.** ANTS-3757 § 2.1 gives `MigrationPlan` no carrier for any of the three: a `blocked-by` edge, a spec citation and a feedback-file reference are all *derived* from prose this half never re-parses. They stay empty after a migration, which a reader checking table coverage would otherwise have to infer from four sections' silence.
- **Writing allocated ids back into `ROADMAP.md`.** § 2.6.1 names it as the durable fix for id-less re-run matching and it is still out of scope: it is a write to the *source*, not the store, and ANTS-3758 owns the source-writing surface.

## 6. Tests

Feature tests under `tests/features/`, label `features;fast`:

| Directory | Covers |
|---|---|
| `roadmap_migrate_load/` | INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-11, INV-12, INV-13, INV-14, INV-15 |
| `roadmap_store_schema/` | INV-7, INV-8, INV-9, INV-10 — the four **store-surface** invariants, filed with the rest of ANTS-3756's write-path invariants rather than in a second directory. INV-9 belongs here with the other three: it constrains `setLegend()`, `addElement()` and `setSectionIntro()`, which are store methods, not loader behaviour. **They are numbered INV-22–25 there and in the test names** (§ 7): that directory already tests an ANTS-3756 INV-7, INV-8 and INV-10 meaning something else entirely. |

All **fifteen** invariants are covered; none is a grep-only check. The new directory adds its `test_*.cpp` to **`test_core`**'s `SOURCES` list, per `tests/features/README.md` step 4 and for the ANTS-1217 reason § 4 gives; no `add_executable`.

Per the project convention (`CLAUDE.md`, `testing.md`), each test is verified to **fail against pre-implementation source** before the implementation is restored. Two are worth naming because the mutation is not the obvious one: INV-2's re-run must be shown red against a loader that rewrites every field on every run (which passes a load-once test), and INV-5's must be shown red against in-place position updates (which pass on a plan whose order did not change).

## 7. Cross-doc impact

- **[ANTS-3756](ANTS-3756-roadmap-store-schema.md) is amended, not merely referenced — done 2026-08-01.** § 2.3 changes `putItem()`'s shipped transaction behaviour — including removing its internal `ROLLBACK` when it does not own the transaction — and adds `begin`/`commit`/`rollback`/`inTransaction`; § 2.4 adds nineteen more plus two structs (`ItemRef`, `SectionRow`). **Twenty-three methods in total land on ANTS-3756's public surface**, and that spec's § 2 and its INV-20 discussion both describe `putItem()` as opening its own transaction, which stops being unconditionally true. The four-argument `setItemField()` is an overload, so its INV-10 is untouched. INV-7, INV-8, INV-9 and INV-10 above are store invariants and are folded into ANTS-3756's own list **as INV-22–25**: that document already carries an INV-7, an INV-8 and an INV-10 meaning something else entirely, so importing these numbers would have left `roadmap_store_schema` with two tests called `Inv7*` testing unrelated things. A store invariant filed only in a migration spec is one nobody looking at the store will find.
- **[ANTS-3757](ANTS-3757-roadmap-migration-read.md)** — two amendments, both applied 2026-08-01. § 2.1.1's last two owed rows (`projectId`, `sectionId`) are discharged here; and **§ 2.10's note-code set, which that spec calls closed, gains this half's seven load-only codes** (`orphaned_item`, `id_allocated`, `history_capped`, `field_conflict`, `project_refused`, `bad_options`, `ambiguous_rematch`). **Seven, and this line said six until implementation counted them**: `ambiguous_rematch` arrived with § 2.6.1 at loop 3 and this sentence was not re-counted — which is precisely the arithmetic drift a vocabulary held in two documents produces, so both now enumerate the codes rather than state a count. `Note::code` is one type shared by both halves.
- **[ANTS-3761](ANTS-3761-roadmap-export-format.md)** — the export must round-trip everything this writes. The `element` rows, `section.intro` and `project.legend` are already in its record set; this is the first writer to produce them from anything but a rebuild, so its INV-2 column diff becomes a real test of this half rather than of the rebuild alone.
- **`docs/standards/roadmap-data-model.md`** — § 7.2's allocation policy is executed here and not restated. No amendment: § 2.8 adds mechanics, not policy.
- **[`docs/subsystems.md`](../subsystems.md)** — gains the `roadmapmigrateload` lane, next to `roadmapmigrate`'s. **`CLAUDE.md` needs no edit and this bullet used to say it did:** the module map moved to `docs/subsystems.md` in ANTS-1292 and `CLAUDE.md` carries only a pointer to it, so a lane added there instead would be one `indie_review_partition` never derives a review lane from.
- **`ROADMAP.md`** — ANTS-3765 flips to in-progress at implementation; ANTS-3758 unblocks on ship.
- **`CHANGELOG.md`** — on ship.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 4-impl | 2026-08-01 | none — implementation, not a review | — | **Implementation row, written by the implementer** (`/cold-eyes` writes only review rows). `src/roadmapmigrateload.{h,cpp}` in `ants_roadmapstore_lib`, 23 methods on `RoadmapStore`, 16 tests — 11 in `roadmap_migrate_load/` and the four store invariants in `roadmap_store_schema/` as ANTS-3756 INV-22–25 (renumbered: that directory already tests an INV-7, INV-8 and INV-10 meaning something else). **Every one was shown RED against the mutation its own *Breaks when* clause names**, in two rounds so each failure is attributable — the loader run outside a transaction, the field comparison rewriting every field, § 2.6.1's natural key removed, orphan re-filing skipped, positions shifted in place, allocation moved before the transaction, the `Access` check dropped, the dry run short-circuited, `seq` restarted per run, and every `appendHistory()` failure treated alike. A test that stays green against its named break tests nothing. **One clause was disproved by building it.** § 2.9 told the loader to tell an at-cap `appendHistory()` refusal from any other failure by comparing `historyBytes()` against `historyCapBytes()`; the store refuses when *stored + incoming* exceeds the cap, so at refusal the stored bytes are still **below** it in every case but an exact landing — and the first refusal against an empty `history` compares 0 with the cap and reads as some other failure, aborting the very project the exception exists to save. INV-15's 8-byte cap is what surfaced it; the loader now re-evaluates the store's own predicate, still with no error text parsed. **Three methods § 2.4 had not declared were added**, each performing an operation the prose mandates and no declared method could carry out — `readSection()`, `idPrefixFor()`, `unfileItem()`; that is the fourth review pass over one list to find this, which § 2.4 now records as the reason its exhaustiveness claim is a traceability rule rather than an adjective. § 2.8 also had to decide `id_origin` for an allocated id, which nothing stated: `synthesised`, the only one of the column's three CHECKed values that is true of an id no source file contained. **And one defect no invariant test could have caught was found by running the real corpus** (§ 4): the plan's synthetic root section carries a *default-constructed* slug and title, which `QSqlQuery` binds as SQL NULL against two NOT NULL columns — so a project with content above its first heading was refused outright, and had the insert succeeded `findSection()` could never have matched it again, since `slug = NULL` is never true and SQLite's UNIQUE treats NULLs as distinct. Every invariant test names its sections, and no named slug is null. Normalised at the write boundary, with a regression test. The corpus run also replaced § 4's provisional 5 s with a measured 1 s, and confirmed INV-2 on real data: seven projects re-run with zero inserts, updates, orphans and allocations, against a corpus ~40% of which is matched through § 2.6.1's natural key rather than by id. Suite 3136/3136 plus 16. |
| 3 | 2026-07-31 | 2 (same packet, cold; no prior-loop briefing) | 2 / 5 / 9 / 8 / 0 | **Converged by cap.** 24 verified, 1 dismissed, 23 fixed. Both lanes again led on the same two CRITICALs, and one of them is a **structural draft defect surfacing only at loop 3** — the skill's own signal to stop looping rather than dispatch a fourth. **§ 2.6 defined re-run matching only for items that carry an id.** § 2.8 allocates ids *inside the store* and § 5 forbids writing back to source, so an id-less bullet arrives id-less on every run: unmatched, re-inserted with a freshly allocated id, its predecessor orphaned. ANTS-3757 § 2.9 counts ~1,600 such items — roughly 40% of the corpus — so a re-run would have duplicated most of it, burnt ~1,600 ids per pass, and INV-2 would have passed anyway because its plan need carry no id-less item. New § 2.6.1 gives them a natural key (same section + `provenance.id == "migrated"` + byte-identical headline), refuses to guess an ambiguous match, states plainly that the key is weaker than an id, and names the durable fix as ANTS-3758's. INV-2 gains the leg that would have caught it. The second CRITICAL was loop-2 collateral of the purest kind: `clearProjectElements(projectId)` was declared while § 2.6 said "the sections the plan carries" — and § 2.1 makes the declaration win, so the signature mandated exactly the silent narration-payload loss the prose forbids. Now `clearSectionElements(sectionId)`. Also fixed: nothing distinguished an at-cap `appendHistory()` refusal (project continues) from any other failure (project aborts) — the discriminator is now `historyBytes()` vs `historyCapBytes()` rather than string-matching an error; the id **rendering** format was never stated, so the padding would have been invented and then permanent; a cleared `body` was specified as SQL NULL with no method able to write one (`clearItemField()` added); step 4 would have called `fileItem()` on orphans that were never unfiled, whose refusal aborts the project; and three writers sat under a `// --- readers ---` heading. **Two claims the lanes could not check from the packet, verified here instead of deferred**: `registerProject()`/`appendHistory()` do not self-transact, and Qt's SQLite driver really does emit a bare `BEGIN` (`strings` on the driver — the evidence is now in § 2.3 rather than asserted). **Dismissed again:** add a TOC — no sibling spec has one. Doc 398 → 446 lines. **Not looping further:** the cap is 3 by decision, and a structural defect appearing this late says two earlier cold reads never reached that part of the document — a fourth read is not the remedy. |
| 2 | 2026-07-31 | 2 (same packet, cold; no prior-loop briefing) | 1 / 4 / 8 / 12 / 0 | 25 verified, **1 dismissed**, 24 fixed. **Origin split: ~8 fix collateral, ~16 draft defects** — draft defects fell 27 → 16 and collateral appeared for the first time, all of it from one loop-1 edit whose blast radius I under-swept. Both lanes again led on the same CRITICAL, and it was that collateral: loop 1 added the sentence "the list is exhaustive" to § 2.4 while §§ 2.6/2.9 mandate four operations no declared method could perform — the element delete (`element` has no `project_id`, so the predicate joins through `section`), the section `title`/`level` update (`addSection()` is INSERT-only), the `MAX(seq)` read (`appendHistory()` takes `seq` from its caller), and — found by one lane only — **item enumeration**, since orphan detection is a set complement and `findItem()` is a point lookup. All four added; the exhaustiveness claim is now stated as a traceability rule, because a list asserted complete twice and falsified twice needs a test rather than an adjective. **Two lane claims were checked against source rather than accepted**: that `registerProject()`/`appendHistory()` might open their own transactions (they do not — both are bare queries, so `putItem()` really is the only self-transacting writer, now stated as the precondition it is), and that § 2.8's "`roadmap_log` derives a prefix from `exportSlug`" was false (**it is** — the verb derives it from `caller_cwd`'s leaf directory; the claim was mine and is corrected). Also fixed: `field_conflict` had no reachable trigger as defined and is re-pointed at § 2.6 rule 3's suppressed-overwrite case; § 2.6's project-wide element delete would have silently destroyed narration payload in retained sections, now scoped to the plan's sections; INV-4's history leg was vacuous (nothing writes history on an initial load); orphan ordering and root-section-on-demand were undefined; the cap exception gained INV-15; `body`'s cleared representation pinned to NULL. **Dismissed:** "add a TOC" — no sibling spec in this corpus carries one and `specs.md` does not ask for it. Doc 360 → 398 lines. |
| 1 | 2026-07-31 | 2 (identical shared packet, cold) | 3 / 5 / 9 / 10 / 2 | 28 verified, **1 dismissed**, all 27 actionable fixed; all draft defects (first gate on this document). **Both lanes independently led on the same three CRITICALs, and all three were the same class: the design named a mechanism the surface it defines cannot perform.** § 2.6's element rebuild had no way to re-file an *existing* item — `addElement()` refuses `kind='item'` and `putItem()` would violate `UNIQUE (project_id, id_fold)` — so every matched item and every orphan ended the transaction unfiled, which is INV-4 and ANTS-3756 INV-20 false at a commit boundary; `fileItem()` added. The re-run's field comparison had nothing to read with (`findItem()` returned only a pk) and its provenance-merge rule was unreachable, because the shipped `setItemField()` hardcodes `asserted`; `readItem()` and a four-argument overload added. And § 2.3 was silent on `putItem()`'s three internal `ROLLBACK` calls — participating unchanged, one bad item would abort the *caller's* transaction from the inside, after which every later write autocommits and persists while `Outcome` reports success: INV-1 inverted with nothing observing it. **One claim was dismissed on verification and it was a CRITICAL in one lane** — both lanes said a re-run collides on the `project` row; `registerProject()` is already get-or-create, selecting on `root` before inserting. The *section* half was real (`addSection()` is a bare INSERT against `UNIQUE (project_id, slug)`), so the finding regraded to HIGH, section-half only. Also fixed: § 2.4's blanket "each canonicalises its JSON" was false for three of four writers and would have had an implementer canonicalise narration prose, which ANTS-3756 § 2.3 calls undefined; INV-3's recipe set `priority` through `setItemField()`, which neither accepts it nor takes an `int`; `history.seq` restarting per run collides on `UNIQUE (item_pk, changed_at, seq)` when two runs share a stamp (now INV-14); § 2.8's prefix derivation was undefined for a project with no ids and for "maximum" over `TEXT`; and an `Outcome::notes` comment said "never a superset" where it meant subset. Doc 267 → 360 lines. |
