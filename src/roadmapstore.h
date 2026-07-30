// ANTS-3756 — the roadmap store: engine, location, schema and connection
// pragmas. Spec: docs/specs/ANTS-3756-roadmap-store-schema.md
//
// The store is PRIMARY, not a cache. It lives under XDG_DATA_HOME (never a
// cache path) and its only rebuild path is the export (ANTS-3761), which is
// why it is created with synchronous=FULL and mode 0600.
#pragma once

#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
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

    explicit RoadmapStore(QString dbPath = QString(),
                          qint64 historyCapBytes = kDefaultHistoryCapBytes);
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
    static QString canonicalJson(const QJsonObject &o);

private:
    bool applyPragmas(QString *error);
    bool createSchema(QString *error);

    QString m_path;
    QString m_connName;
    QSqlDatabase m_db;
    qint64 m_historyCap;
};
