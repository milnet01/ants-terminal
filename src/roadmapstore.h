// ANTS-3756 — the roadmap store: engine, location, schema and connection
// pragmas. Spec: docs/specs/ANTS-3756-roadmap-store-schema.md
//
// The store is PRIMARY, not a cache. It lives under XDG_DATA_HOME (never a
// cache path) and its only rebuild path is the export (ANTS-3761), which is
// why it is created with synchronous=FULL and mode 0600.
#pragma once

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

    // Schema version carried in PRAGMA user_version; the export's `meta`
    // record carries the same number.
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

    // INV-8 — a project is keyed on its CANONICAL root. A path that cannot be
    // canonicalised is REFUSED, never stored: QFileInfo::canonicalFilePath()
    // returns an empty string for a non-existent path, and '' under
    // `root TEXT UNIQUE` would fuse every such project into one.
    std::optional<qint64> registerProject(const QString &root, const QString &name,
                                          const QString &exportSlug,
                                          QString *error = nullptr);

    std::optional<qint64> addSection(qint64 projectId, const QString &slug,
                                     const QString &title, int level,
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
                       std::optional<qint64> parentId, QString *error = nullptr);

    // element rows that are NOT items. Refuses kind='item' outright rather than
    // letting the DDL CHECK decide, so putItem()/fileItem() stay the only ways
    // an item is filed (INV-10, INV-20).
    bool addElement(qint64 sectionId, int position, const QString &kind,
                    const QString &payload, QString *error = nullptr);

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
    };
    std::optional<SectionRow> readSection(qint64 sectionId, QString *error = nullptr) const;

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
