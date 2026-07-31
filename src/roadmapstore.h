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
};
