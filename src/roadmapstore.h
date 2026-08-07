// ANTS-3756 — the roadmap store: engine, location, schema and connection
// pragmas. Spec: docs/specs/ANTS-3756-roadmap-store-schema.md
//
// The store is PRIMARY, not a cache. It lives under XDG_DATA_HOME (never a
// cache path) and its only rebuild path is the export (ANTS-3761), which is
// why it is created with synchronous=FULL and mode 0600.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <optional>

class RoadmapStore {
public:
    // INV-14 — the history bound is store-wide and INJECTABLE. The production
    // figure is 250 MiB; a test that reached it honestly would take minutes and
    // a disk, so the bound is a constructor parameter. A cap reachable only in
    // production is a cap nothing exercises.
    static constexpr qint64 kDefaultHistoryCapBytes = 250LL * 1024 * 1024;

    // Schema version carried in PRAGMA user_version — the shape of THESE
    // TABLES. The export's `meta` record carries its own, independent number
    // (RoadmapExport::kExportSchemaVersion): it describes the JSONL record
    // shape, and ANTS-3781 § 2.3 held the two apart so a table-shape bump does
    // not invalidate every export ever written.
    static constexpr int kSchemaVersion = 1;

    // INV-16 — the write deadline, in ms, matching ConfigWriteLock's rather
    // than introducing a second timeout constant. One number covers both the
    // connection pragma and enableWal()'s own retry.
    static constexpr int kBusyTimeoutMs = 5000;

    // The bulk deadline, for a writer that KNOWS it may queue behind a long
    // transaction — migration (ANTS-3757) and export. 30 s is RetroDB's
    // figure, arrived at there after "database is locked" under concurrent
    // bulk jobs. INV-16 is unchanged by it: both profiles still fail and
    // report at their deadline, neither retries silently. A single 30 s
    // deadline everywhere was rejected — an interactive roadmap edit that
    // hangs for half a minute before erroring reads as a freeze, not an error.
    static constexpr int kBulkBusyTimeoutMs = 30000;

    // Which deadline and cache profile a connection opens with.
    enum class Access { Interactive, Bulk };

    // Bounds the WAL sidecar, which otherwise keeps whatever high-water mark
    // one large transaction gave it. Connection-scoped despite reading like a
    // file setting — measured (set, reconnect, read back: -1).
    static constexpr qint64 kJournalSizeLimitBytes = 64LL * 1024 * 1024;

    // Page cache for the Bulk profile only, in KiB. Interactive stays on
    // SQLite's 2 MiB default so ANTS-3761 INV-12's 4 MiB export budget holds.
    static constexpr int kBulkCacheKiB = 16 * 1024;

    explicit RoadmapStore(QString dbPath = QString(),
                          qint64 historyCapBytes = kDefaultHistoryCapBytes,
                          Access access = Access::Interactive);
    ~RoadmapStore();

    RoadmapStore(const RoadmapStore &) = delete;
    RoadmapStore &operator=(const RoadmapStore &) = delete;

    // INV-7 — GenericDataLocation + "/ants-terminal/roadmap.sqlite", never a
    // cache root. GenericDataLocation and not AppDataLocation: there is no
    // setOrganizationName in src/, so AppDataLocation resolves to a directory
    // with a space in it that no other writer in this project uses.
    static QString defaultPath();

    bool open(QString *error = nullptr);
    bool isOpen() const { return m_db.isOpen(); }

    // ANTS-3765 § 2.3 — explicit transaction control, so a caller can make one
    // project one transaction. putItem()'s self-committing shape made that
    // unexpressible in both directions: SQLite refuses a nested BEGIN, so a
    // wrapper's first putItem() failed and the migration wrote nothing; drop
    // the wrapper and a failure at item 400 of 600 leaves 399 committed.
    // QSqlDatabase::transaction() is deliberately not used — that driver emits
    // a DEFERRED `BEGIN` (checked with `strings` on libqsqlite.so), which takes
    // its write lock at the first write and can therefore fail SQLITE_BUSY
    // mid-load, against a design whose whole concurrency story is the lock
    // being held from the start (§ 2.5, INV-16).
    //
    // The flag is a member because SQLite exposes autocommit state through
    // sqlite3_get_autocommit(), which QSqlDatabase does not surface.
    bool begin(QString *error = nullptr);     // BEGIN IMMEDIATE; refuses to nest
    bool commit(QString *error = nullptr);    // refuses when none is open
    bool rollback(QString *error = nullptr);  // refuses when none is open
    bool inTransaction() const { return m_inTransaction; }

    // ANTS-3765 § 2.2 — which deadline and cache profile this connection
    // opened with. A migration against an Interactive store is REFUSED rather
    // than run: a 5 s deadline against a migration-sized transaction fails
    // *sometimes*, which is the worst available behaviour.
    Access access() const { return m_access; }

    // INV-15 — did THIS open create the schema? Two processes opening a store
    // that does not exist must produce exactly one creator, and after the fact
    // the winner and the loser are otherwise indistinguishable: both end with
    // user_version = 1 and one set of tables. Without this the CREATE TABLE IF
    // NOT EXISTS design the invariant forbids passes its own test.
    bool createdSchema() const { return m_createdSchema; }
    QString path() const { return m_path; }
    QSqlDatabase &db() { return m_db; }

    // ANTS-3781 § 2.1 — the upgrade ladder. One rung of it: the statements that
    // take a store from version `to - 1` to version `to`, in the order given.
    struct Upgrade {
        int to;
        QStringList statements;
    };

    // Climbs `from` to `to`, applying exactly one rung per version step, and
    // stamps `PRAGMA user_version = to` once after the last rung.
    //
    // TWO PASSES, and the split is the contract rather than an implementation
    // detail (INV-2). Pass one validates the WHOLE range and executes nothing:
    // every version in (from, to] must have exactly one rung, and any other
    // count refuses naming that version. Pass two then runs the rungs in
    // ascending version order. A single pass that looked each rung up as it
    // reached it would run rung 2's statements before discovering rung 3 was
    // missing.
    //
    // PRECONDITION: the caller has an open transaction. This function neither
    // begins nor commits one — a half-applied upgrade to a store whose only
    // rebuild path is the export is the worst outcome available, so the
    // atomicity belongs to one owner and that owner is createSchema(). The
    // stamp is durable only when the caller commits. There is no runtime guard
    // for the precondition: QSqlDatabase does not surface
    // sqlite3_get_autocommit() (the same limitation m_inTransaction exists
    // for), so the check cannot be made without inventing a second one.
    //
    // Degenerate arguments, IN THIS ORDER, because two of the rules overlap and
    // the order is what makes the result single-valued:
    //   1. `from < 1` REFUSES, whatever `to` is. Version 0 is a store with no
    //      schema at all and creating one is the DDL's job, not a rung's; a
    //      negative version is a corrupt pragma. This arm is why createSchema()
    //      can route every non-zero version here and still fail legibly.
    //   2. otherwise `from >= to` is an empty climb — nothing validated,
    //      nothing run, nothing stamped, returns true. Note this makes a
    //      DOWNGRADE range a silent no-op rather than a refusal; refusing a
    //      downgrade is createSchema()'s `version > kSchemaVersion` arm and
    //      stays there.
    //   3. otherwise the two passes above run over `(from, to]`.
    // `to` is NOT compared to kSchemaVersion — a test climbs past it by design
    // (INV-1), and this function's job is the climb, not the policy.
    // Rungs whose TARGET VERSION (Upgrade::to) falls outside the climbed range
    // are ignored. A rung with an empty `statements` list is legal — it
    // satisfies the exactly-one-rung requirement for its version without
    // running SQL; the version still moves only at the final stamp, never per
    // rung.
    //
    // `from` is trusted rather than re-read. Not because a re-read would race —
    // inside the caller's write lock it could not — but because the version is
    // an argument for the same reason the ladder is: a function that read its
    // own starting point could only ever be tested against a store already at
    // it.
    //
    // Public, and taking its ladder as an argument, for the reason
    // kDefaultHistoryCapBytes is a constructor parameter (INV-14): at
    // kSchemaVersion 1 the production ladder is EMPTY, so a ladder reachable
    // only from production is a ladder nothing can exercise until the first
    // bump.
    static bool applyUpgrades(QSqlDatabase &db, int from, int to,
                              const QVector<Upgrade> &ladder, QString *error = nullptr);

    // The production ladder — empty at kSchemaVersion 1, because there is no
    // version below it to climb from. The first rung lands with the first bump
    // (ANTS-3815); INV-4 is what makes that a red test rather than a thing to
    // remember.
    //
    // A public static and not a file-scope object in the .cpp: INV-4's
    // completeness check compiles into another translation unit, and a ladder
    // that check cannot reach leaves the missing-rung case — the one INV-2
    // refuses at runtime — with nothing checking for it at build time, which is
    // the whole point of having INV-4.
    static const QVector<Upgrade> &upgradeLadder();

    // INV-8 — a project is keyed on its CANONICAL root. A path that cannot be
    // canonicalised is REFUSED, never stored: QFileInfo::canonicalFilePath()
    // returns an empty string for a non-existent path, and '' under
    // `root TEXT UNIQUE` would fuse every such project into one.
    std::optional<qint64> registerProject(const QString &root, const QString &name,
                                          const QString &exportSlug,
                                          QString *error = nullptr);

    // ANTS-3796 § 2.3 — `position` is a required parameter and not a setter,
    // unlike setSectionIntro()/setSectionSource(): those columns are nullable
    // and were added to a shipped signature, this one is NOT NULL with no
    // default, so an insert omitting it could not succeed and a setter would be
    // unreachable. Placed BEFORE the defaulted parentId deliberately — every
    // existing call site then fails to compile rather than silently rebinding
    // its parent argument onto the new parameter.
    std::optional<qint64> addSection(qint64 projectId, const QString &slug,
                                     const QString &title, int level, int position,
                                     std::optional<qint64> parentId = std::nullopt,
                                     QString *error = nullptr);

    struct ItemWrite {
        qint64 projectId = 0;
        QString id;
        QString idOrigin = QStringLiteral("parsed");
        QString status;
        QString headline;
        QString kind;
        QString source;
        QString layman, resolution, body, milestone;
        std::optional<int> priority;
        QString visibility = QStringLiteral("public");
        QString created, lastModified, shipped;
        // ANTS-3767 — the three JSON columns had a DDL column each and no way
        // to reach them, so through the public API they could only ever hold
        // their defaults. `lanes`/`evidence` are the `Lanes:`/`Evidence:` lines
        // (roadmap-data-model.md § 4.1); `extras` is § 4.3's extension tail.
        // All three are canonicalised on the way in — § 2.3's rule is about the
        // STORED bytes, so it binds every writer, not just the export.
        QStringList lanes, evidence;
        QJsonObject extras;
        QJsonObject provenance;
        // INV-20 — an item is filed by its element row, and putItem() creates
        // exactly one. There is no item.section column: order and filing live
        // once, in element.
        qint64 sectionId = 0;
        int position = 0;
    };
    std::optional<qint64> putItem(const ItemWrite &w, QString *error = nullptr);

    // INV-10 — provenance is per FIELD in both directions: writing `headline`
    // sets provenance.headline to `asserted` and leaves every other key alone.
    bool setItemField(qint64 itemPk, const QString &field, const QString &value,
                      QString *error = nullptr);

    // ANTS-3765 § 2.4 — the same write, with the provenance value as a
    // parameter. `asserted` is right for a human edit and wrong for every write
    // migration makes: it records `migrated` and `defaulted` (ANTS-3757
    // § 2.7–2.9), and losing that distinction collapses the migrated-versus-
    // asserted difference the model's § 3.1 write-tier gate rests on. An
    // overload, so the 3-argument form keeps its meaning and INV-10 is
    // untouched.
    bool setItemField(qint64 itemPk, const QString &field, const QString &value,
                      const QString &provenance, QString *error = nullptr);

    // ANTS-3765 § 2.6 — writes SQL NULL. putItem() binds an empty `body` as
    // NULL while setItemField() binds the QString it is given, so `''` and NULL
    // are the same call there; without this the same logical state has two
    // representations depending on which path last touched it, and ANTS-3761's
    // INV-2 column diff sees a difference where there is none.
    bool clearItemField(qint64 itemPk, const QString &field, const QString &provenance,
                        QString *error = nullptr);

    // INV-6 — `relates-to` is symmetric and stored ONCE, normalised on stable
    // identity (export_slug, id_fold) — never on a rowid, which a rebuild
    // reassigns. Normalisation applies only when both endpoints resolve.
    bool relateItems(const QString &type, qint64 srcPk, qint64 dstPk,
                     QString *error = nullptr);
    bool relateCrossProject(const QString &type, qint64 srcPk,
                            const QString &dstProject, const QString &dstIdFold,
                            QString *error = nullptr);

    // INV-14 — below the cap nothing is ever evicted; at the cap the history
    // write FAILS AND REPORTS while the item write it accompanies still
    // succeeds. A silently dropped revision is indistinguishable from one that
    // never happened.
    bool appendHistory(qint64 itemPk, const QString &changedAt, int seq,
                       const QString &field, const QString &oldValue,
                       const QString &newValue, QString *error = nullptr);
    qint64 historyBytes() const;
    qint64 historyCapBytes() const { return m_historyCap; }

    // --- ANTS-3765 § 2.4 — what the migration load half needs -----------------
    // Every method here is traceable to the section that needs it, and that
    // traceability is the contract: an operation a later section names with no
    // method to perform it is a defect in the list, because the only other way
    // to perform it is raw SQL at the call site — which would make the loader a
    // third producer of these rows after putItem() and ANTS-3761's rebuild.
    //
    // Only setLegend() and addElement(kind='table') canonicalise. Stated per
    // method rather than as a blanket rule: setSectionIntro() writes prose,
    // raiseIdHighWater() writes an integer, and narration payload is prose that
    // § 2.3 forbids canonicalising ("undefined rather than merely wasteful").

    // --- writers ---

    // section.intro — addSection() has no argument for it. Stored VERBATIM.
    bool setSectionIntro(qint64 sectionId, const QString &intro, QString *error = nullptr);

    // ANTS-3782 § 2.2 — section.source_path, which source file this section was
    // read from. nullopt writes SQL NULL and means the live roadmap.
    //
    // A separate setter rather than a wider addSection(), exactly as
    // setSectionIntro() is: ANTS-3765 § 2.6 resolves a section and then writes
    // the fields that differ, so the write has to reach an EXISTING row, which
    // an INSERT-only addSection() cannot offer.
    //
    // An ENGAGED optional holding an empty string stores '' and does NOT fold
    // to NULL — which is where this parts company with setSectionIntro()
    // directly above. '' is a meaningless intro and a WRONG source path, so
    // folding it would make "unplaceable, stored anyway" read back as "the live
    // roadmap" and collapse the one distinction the optional exists to carry.
    // RoadmapMigrateLoad::load() never produces the value (ANTS-3782 § 2.4
    // refuses it first), so this binds a second caller rather than that one.
    bool setSectionSource(qint64 sectionId, const std::optional<QString> &sourcePath,
                          QString *error = nullptr);

    // § 2.6's section update: addSection() is INSERT-only and a re-run can
    // change a heading's title, level or parent. Takes the whole tuple — they
    // come from one PlannedSection and a partial update has no meaning.
    bool updateSection(qint64 sectionId, const QString &title, int level,
                       int position, std::optional<qint64> parentId,
                       QString *error = nullptr);

    // element rows that are NOT items. Refuses kind='item' outright rather than
    // letting the DDL CHECK decide, so putItem()/fileItem() stay the only ways
    // an item is filed (INV-10, INV-20).
    bool addElement(qint64 sectionId, int position, const QString &kind,
                    const QString &payload, QString *error = nullptr);

    // ANTS-3809 § 2.2 — the read-modify-write `bundle_row` needs and the
    // declared surface could not express: addElement() is INSERT-only,
    // clearSectionElements() is the whole section, and ElementRow carries no
    // element id, so there was no way to reach ONE element's payload.
    //
    // Keyed on (section_id, position) because that pair is UNIQUE and is what
    // listElements() already hands back; exposing an element_id would be a
    // wider surface than one op needs (§ 5). Takes no `kind`: the row's own
    // kind decides whether the payload is canonicalised, exactly as
    // addElement() does for the kind it was given.
    //
    // Refuses kind='item' for addElement()'s reason rather than its check —
    // that one rejects its `kind` ARGUMENT, this one the row already there —
    // so putItem()/fileItem() stay the only ways an item is filed (INV-10,
    // INV-20). Refuses a (section_id, position) with no row: there is nothing
    // to modify, and inserting instead would make this a second addElement()
    // that skips its kind='item' guard.
    bool setElementPayload(qint64 sectionId, int position, const QString &payload,
                           QString *error = nullptr);

    // § 2.6's element rebuild re-files items that already exist, which putItem()
    // cannot do (UNIQUE (project_id, id_fold)) and addElement() must not.
    // Refuses an already-filed item: INV-20's "at most one" is elem_item_uq,
    // and leaning on it here turns a reportable refusal into a constraint
    // violation that aborts the caller's whole migration.
    bool fileItem(qint64 itemPk, qint64 sectionId, int position, QString *error = nullptr);

    // The inverse, for the one case § 2.6's rebuild cannot otherwise reach: an
    // item that is still in source but whose STORED section the plan no longer
    // carries — a heading deleted while its bullets moved elsewhere. Step 2
    // clears only the plan's sections, so that item's old element row survives,
    // fileItem() then refuses it as already filed, and § 2.5 turns the refusal
    // into a rolled-back project. Clearing its retained section instead would
    // destroy narration payload nothing re-inserts, so the delete has to be per
    // ITEM. Removes the kind='item' row; the item itself is untouched.
    bool unfileItem(qint64 itemPk, QString *error = nullptr);

    // § 2.6 step 2's element delete, PER SECTION and never per project: a
    // section retained because the plan no longer carries it also holds
    // narration and table rows nothing re-inserts.
    bool clearSectionElements(qint64 sectionId, QString *error = nullptr);

    // project.legend — one JSON object per project (roadmap-data-model.md
    // § 5.1). CANONICALISED.
    bool setLegend(qint64 projectId, const QJsonObject &legend, QString *error = nullptr);

    // id_prefix high-water. Advances only UPWARD: an id this migration
    // allocated must never be reissued by a later run (§ 2.8). A value at or
    // below the stored one is a no-op, not an error.
    bool raiseIdHighWater(qint64 projectId, const QString &prefix, qint64 highWater,
                          QString *error = nullptr);

    // --- readers ---

    // § 2.6 re-run matching — resolve an id within one project, folded. The
    // error out-param is not decoration: without it `nullopt` conflates "no
    // such item" with "the query failed", and the failure path of that
    // confusion is an INSERT of an item that already exists.
    std::optional<qint64> findItem(qint64 projectId, const QString &id,
                                   QString *error = nullptr) const;

    // § 2.6's field comparison, returned in the SAME type the writer takes so
    // "compare, then write the difference" needs no second shape. ItemWrite
    // already carries sectionId/position, which is the current filing § 2.7
    // must capture BEFORE the element rebuild deletes it.
    std::optional<ItemWrite> readItem(qint64 itemPk, QString *error = nullptr) const;

    // ANTS-3816 — every item of one project in ONE query, keyed by item_pk.
    // ANTS-3793 § 4 named this as the remedy if its p95 budget reds and said to
    // build it only then; it red, and this is the measurement that earned it:
    // a 1,839-item project cost 83.4 ms through readItem() per item against a
    // 50 ms budget, with the render-and-parse half of the same read costing
    // 8.6 ms (2026-08-04). The N+1 was 90% of the work.
    //
    // A QHash and not a QVector because both callers — the read seam's walk and
    // the render's — resolve an element's item_pk against it rather than
    // iterating. RoadmapRender::render() builds exactly this hash by hand today.
    std::optional<QHash<qint64, ItemWrite>> readItems(qint64 projectId,
                                                      QString *error = nullptr) const;

    // One enumeration serving BOTH § 2.7's orphan detection (a set complement)
    // and § 2.6.1's id-less re-run matching (a search by natural key). Neither
    // is expressible as a point lookup however many times it is called.
    //
    // idFold and not id: matching is case-folded within a project (INV-3), so a
    // set difference over raw ids reports a stored `SH-1` as an orphan of a
    // planned `Sh-1`. idFromMigration is provenance.id == "migrated" — only
    // rows this migration gave an id to are eligible for a headline re-match.
    struct ItemRef {
        qint64  itemPk = 0;
        QString idFold, headline;
        qint64  sectionId = 0;   // 0 when unfiled (transiently, mid-rebuild)
        bool    idFromMigration = false;
    };
    std::optional<QVector<ItemRef>> listItems(qint64 projectId,
                                              QString *error = nullptr) const;

    // Section resolution on a re-run. addSection() is a bare INSERT and
    // collides on UNIQUE (project_id, slug) the second time. registerProject()
    // needs no equivalent — it is already get-or-create.
    std::optional<qint64> findSection(qint64 projectId, const QString &slug,
                                      QString *error = nullptr) const;

    // The section half of § 2.6's "written only if it differs". Found missing
    // at implementation: § 2.6 gives items readItem() for exactly this
    // comparison and gave sections nothing, so Outcome::sectionsWritten —
    // declared as counting the sections that CHANGED — was not computable
    // through the declared surface.
    struct SectionRow {
        QString slug, title, intro;
        int     level = 0;
        std::optional<qint64> parentId;
        // ANTS-3782 § 2.3 — nullopt = the live roadmap. std::optional rather
        // than an empty QString, matching parentId in this same struct: the
        // NULL / '' distinction is load-bearing, and a type that could not
        // express it would lose the distinction at the reader. Without this
        // field the column would be write-only and INV-14 could not observe it.
        std::optional<QString> sourcePath;
        // ANTS-3796 § 2.1 — document order within the project: the sequence the
        // render emits sections in, and the only record of it. Read here for
        // the same reason sourcePath is: § 2.6's "written only if it differs"
        // cannot see a section that MOVED without it, and Outcome::
        // sectionsWritten would stop counting a reorder as a change.
        int position = 0;
    };
    std::optional<SectionRow> readSection(qint64 sectionId, QString *error = nullptr) const;

    // ANTS-3796 § 2.3.1 — the enumerator sectionOrderLess() sorts through.
    // readSection() is a point lookup and findSection() resolves one slug, so
    // neither can produce the SET a sort key applies to; without this every
    // caller would SELECT section_id in raw SQL and read the rows back one at a
    // time, which is the reach-past-the-reader this surface exists to prevent.
    // Shaped after listItems() directly above, and for the same reason.
    std::optional<QVector<SectionRow>> listSections(qint64 projectId,
                                                    QString *error = nullptr) const;

    // ANTS-3758 § 2.1 — the ordered contents of one section. The render's
    // per-section input, and the reader roadmapexport.cpp's writeElements()
    // stops hand-rolling in SQL. Without it there was no element enumerator at
    // all: the export reached past this surface with its own LEFT JOIN, which
    // is exactly the drift listSections()' comment above exists to prevent.
    struct ElementRow {
        int     position = 0;
        QString kind;                 // 'item' | 'narration' | 'table'
        // std::optional and not QString: the DDL makes payload NULL exactly
        // when kind is 'item', and a bare QString collapses NULL with '' — the
        // same distinction SectionRow::sourcePath uses std::optional to keep.
        std::optional<QString> payload;
        qint64  itemPk = 0;           // 'item' only; 0 otherwise
        // The export emits the CASE-FOLDED ref and ItemWrite does not carry it
        // (id_fold is on ItemRef). Without this field the refit would owe a
        // readItem() per element to recover what one LEFT JOIN already returns.
        QString itemIdFold;           // 'item' only; empty otherwise
    };
    std::optional<QVector<ElementRow>> listElements(qint64 sectionId,
                                                    QString *error = nullptr) const;

    // ANTS-3758 § 2.1 — the project row. Same gap as listElements(): the export
    // hand-rolled `SELECT project_id, name, legend FROM project` because no
    // reader existed, and § 2.8's preamble (H1 + status legend) cannot be built
    // without one.
    struct ProjectRow {
        qint64  projectId = 0;
        QString name, exportSlug;
        // The RAW stored text, not a parsed QJsonObject: the export reads it as
        // a string inside a byte-identity contract, so a reader that parsed and
        // re-serialised it would put a round-trip through the middle of INV-1.
        QString legendText;
        // ANTS-3793 § 2.2 — the canonical root INV-8 keys the project on. No
        // consumer of readProjectByRoot() reads it back (that reader returns
        // only the id), so this field is justified by the TEST: ANTS-3793's
        // Inv1DispatchMarker asserts that a symlinked or non-normalised path
        // resolves to the same row as its canonical form, and it cannot make
        // that assertion against a struct that does not carry the value being
        // canonicalised.
        QString root;
    };
    // Two lookups because the two callers key differently, and one of them is
    // the refit: the render holds a projectId; the export does NOT — writeMeta()
    // resolves WHERE export_slug = ? and OBTAINS the id as an output. A
    // projectId-only reader could not serve it, which would leave ANTS-3758
    // INV-11's `FROM project` clause unsatisfiable.
    std::optional<ProjectRow> readProject(qint64 projectId, QString *error = nullptr) const;
    std::optional<ProjectRow> readProjectBySlug(const QString &exportSlug,
                                                QString *error = nullptr) const;

    // ANTS-3793 § 2.2 — the third key, and the one the read seam's dispatch
    // marker needs: "has this project been migrated?" is asked of a project
    // ROOT, which is what INV-8 keys a project on. Until now `root TEXT UNIQUE`
    // was write-only through this surface — registerProject() wrote it and no
    // reader could take it back — so the marker ANTS-3765 § 2.10 defines had
    // nothing able to read it.
    //
    // Takes an ALREADY-canonical path. Canonicalising here would hide the one
    // failure that matters from the caller: QFileInfo::canonicalFilePath()
    // returns empty for a path that does not resolve, and an empty key would
    // silently match nothing and read as "not migrated" — the silent fallback
    // ANTS-3793 INV-1 exists to forbid. RoadmapSource::migratedProject() does
    // the canonicalisation and refuses the empty result outright.
    std::optional<ProjectRow> readProjectByRoot(const QString &canonicalRoot,
                                                QString *error = nullptr) const;

    // § 2.8 step 1's first branch: the prefix this project already allocates
    // under, which idHighWater() cannot answer because it takes the prefix as
    // an argument. Also found missing at implementation. nullopt = no row.
    std::optional<QString> idPrefixFor(qint64 projectId, QString *error = nullptr) const;

    // § 2.8's starting point. Absent row ⇒ nullopt, which is not an error.
    std::optional<qint64> idHighWater(qint64 projectId, const QString &prefix,
                                      QString *error = nullptr) const;

    // § 2.9's seq continuation: appendHistory() takes `seq` from its caller, so
    // the caller needs the current maximum for this (item, stamp). Absent rows
    // ⇒ nullopt, not an error — and the first row of a stamp is seq 0.
    std::optional<int> maxHistorySeq(qint64 itemPk, const QString &changedAt,
                                     QString *error = nullptr) const;

    // Canonical JSON for the store's own column writes. ANTS-3761 owns the
    // general RFC 8785 writer and INV-19 tests it against the RFC's vectors;
    // this covers the shapes the store itself writes (flat objects and arrays
    // of strings), where QJsonObject's sorted keys plus Compact output already
    // agree with JCS byte for byte.
    // Takes a QJsonValue, not a QJsonObject: `lanes` and `evidence` are JSON
    // ARRAYS and are held canonical by the same rule (ANTS-3767). Every
    // existing caller passes an object, which converts implicitly.
    static QString canonicalJson(const QJsonValue &o);

private:
    bool applyPragmas(QString *error);
    bool enableWal(QString *error);
    bool createSchema(QString *error);

    QString m_path;
    QString m_connName;
    QSqlDatabase m_db;
    qint64 m_historyCap;
    Access m_access;
    bool m_createdSchema = false;
    // ANTS-3765 § 2.3 — whose transaction is open. putItem() reads this to
    // decide whether it owns the transaction it is writing in.
    bool m_inTransaction = false;
};

// ANTS-3796 § 2.2 — the section sort key, (position, slug). A free function
// beside SectionRow rather than a member: it compares two rows and touches no
// store state. It has a named home because a sort key with no owner is one
// every caller re-implements — ANTS-3758's render is the production caller, and
// this spec's tests are the first.
//
// The tie-break is not decoration. § 2.1 leaves position distinctness to a
// writer's obligation rather than a UNIQUE constraint (a re-run that swaps two
// sections would collide mid-update, and SQLite defers only foreign keys), so a
// duplicate position must yield a WRONG BUT STABLE order rather than an
// unstable one. `slug` is already UNIQUE (project_id, slug), so it is a true
// tie-break and not a second ambiguity.
//
// QString::compare() is UTF-16 code-unit order, matching how roadmapexport.cpp
// sorts slugs (its cmpCodeUnit()); SQLite's BINARY collation is UTF-8 byte
// order and the two disagree on supplementary-plane characters, which emoji in
// a heading slug reach. That is why this is a C++ comparator and not ORDER BY.
bool sectionOrderLess(const RoadmapStore::SectionRow &a,
                      const RoadmapStore::SectionRow &b);
