// ANTS-3756 — roadmap store implementation.
// Spec: docs/specs/ANTS-3756-roadmap-store-schema.md
#include "roadmapstore.h"
#include "secureio.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

#include <atomic>

namespace {

// The 21 Kind values of roadmap-data-model.md § 7.4. Written as a CHECK, not a
// comment: § 7.4 says "Writes accept canonical values only", and a TEXT NOT
// NULL column accepts anything (INV-11).
constexpr const char *kKindCheck =
    "'implement','fix','audit-fix','review-fix','doc','doc-fix','refactor',"
    "'test','chore','release','perf','security','feature','enhancement',"
    "'investigate','research','accessibility','optimize','package',"
    "'marketing','ux'";

bool exec(QSqlDatabase &db, const QString &sql, QString *error) {
    QSqlQuery q(db);
    if (q.exec(sql))
        return true;
    if (error)
        *error = q.lastError().text() + QStringLiteral(" [") + sql.left(120) + QStringLiteral("]");
    return false;
}

QString lastErr(const QSqlQuery &q) { return q.lastError().text(); }

} // namespace

RoadmapStore::RoadmapStore(QString dbPath, qint64 historyCapBytes)
    : m_path(dbPath.isEmpty() ? defaultPath() : std::move(dbPath)),
      m_historyCap(historyCapBytes) {
    // One QSqlDatabase connection name per instance: two stores in one process
    // (the tests open several) must not share a connection.
    static std::atomic<quint64> counter{0};
    m_connName = QStringLiteral("ants_roadmapstore_%1").arg(counter.fetch_add(1));
}

RoadmapStore::~RoadmapStore() {
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connName);
}

QString RoadmapStore::defaultPath() {
    // GenericDataLocation, never AppDataLocation, and never a cache root.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
        QStringLiteral("/ants-terminal");
    return dir + QStringLiteral("/roadmap.sqlite");
}

QString RoadmapStore::canonicalJson(const QJsonObject &o) {
    // QJsonObject holds keys sorted, and Compact emits no insignificant
    // whitespace — which is JCS for the shapes this store writes.
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

bool RoadmapStore::applyPragmas(QString *error) {
    // Applied on EVERY connection at open, not once at creation.
    // foreign_keys is per-connection and defaults OFF, so without it every
    // REFERENCES in the schema is decorative.
    const char *pragmas[] = {
        "PRAGMA journal_mode = WAL",   // persistent; re-asserting is harmless
        "PRAGMA foreign_keys = ON",    // per-connection; OFF by default
        "PRAGMA synchronous  = FULL",  // primary store, not a cache
        "PRAGMA busy_timeout = 5000",  // ms; matches ConfigWriteLock's deadline
    };
    for (const char *p : pragmas) {
        QSqlQuery q(m_db);
        if (!q.exec(QString::fromLatin1(p))) {
            if (error)
                *error = lastErr(q);
            return false;
        }
    }
    return true;
}

bool RoadmapStore::open(QString *error) {
    const QFileInfo fi(m_path);
    if (!QDir().mkpath(fi.absolutePath())) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(fi.absolutePath());
        return false;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connName);
    m_db.setDatabaseName(m_path);
    if (!m_db.open()) {
        // A Qt SQL build with no QSQLITE driver fails HERE, not at link time.
        if (error)
            *error = m_db.lastError().text();
        return false;
    }
    if (!applyPragmas(error))
        return false;
    if (!createSchema(error))
        return false;

    // INV-17 — the store and BOTH WAL sidecars are 0600. They carry the same
    // content, including visibility:internal items, so securing only the main
    // file would be theatre. The sidecars exist only while a connection is
    // open and a write has happened, so this runs after createSchema().
    setOwnerOnlyPerms(m_path);
    setOwnerOnlyPerms(m_path + QStringLiteral("-wal"));
    setOwnerOnlyPerms(m_path + QStringLiteral("-shm"));
    return true;
}

bool RoadmapStore::createSchema(QString *error) {
    // Creation is itself a race: two processes finding no store both run the
    // DDL. BEGIN IMMEDIATE takes the write lock up front (a deferred
    // transaction that reads then writes must upgrade, and SQLite returns
    // SQLITE_BUSY on that upgrade WITHOUT honouring busy_timeout).
    if (!exec(m_db, QStringLiteral("BEGIN IMMEDIATE"), error))
        return false;

    // The winner is decided by reading user_version INSIDE this transaction.
    // CREATE TABLE IF NOT EXISTS succeeds for both and reports nothing, so it
    // cannot be the discriminator (INV-15).
    int version = 0;
    {
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next())
            version = q.value(0).toInt();
    }

    if (version > kSchemaVersion) {
        exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
        if (error)
            *error = QStringLiteral("store schema %1 is newer than this build's %2")
                         .arg(version).arg(kSchemaVersion);
        return false;
    }
    if (version == kSchemaVersion) {
        return exec(m_db, QStringLiteral("COMMIT"), error);
    }

    const QString ddl[] = {
        QStringLiteral(R"(CREATE TABLE project (
  project_id   INTEGER PRIMARY KEY,
  root         TEXT UNIQUE,
  name         TEXT NOT NULL,
  export_slug  TEXT NOT NULL UNIQUE
                 CHECK (export_slug GLOB '[a-z0-9]*'
                    AND export_slug NOT GLOB '*[^a-z0-9-]*'),
  legend       TEXT NOT NULL DEFAULT '{}'
))"),
        QStringLiteral(R"(CREATE TABLE id_prefix (
  project_id   INTEGER NOT NULL REFERENCES project(project_id),
  prefix       TEXT NOT NULL,
  high_water   INTEGER NOT NULL,
  PRIMARY KEY (project_id, prefix)
))"),
        QStringLiteral(R"(CREATE TABLE section (
  section_id  INTEGER PRIMARY KEY,
  project_id  INTEGER NOT NULL REFERENCES project(project_id),
  slug        TEXT NOT NULL,
  title       TEXT NOT NULL,
  level       INTEGER NOT NULL,
  intro       TEXT,
  parent_id   INTEGER REFERENCES section(section_id),
  UNIQUE (project_id, slug)
))"),
        QStringLiteral(R"(CREATE TABLE item (
  item_pk      INTEGER PRIMARY KEY,
  project_id   INTEGER NOT NULL REFERENCES project(project_id),
  id           TEXT NOT NULL,
  id_fold      TEXT GENERATED ALWAYS AS (lower(id)) VIRTUAL,
  id_origin    TEXT NOT NULL CHECK (id_origin IN
                 ('parsed','synthesised','quarantined')),
  status       TEXT NOT NULL CHECK (status IN
                 ('planned','in-progress','shipped','considered','dropped')),
  headline     TEXT NOT NULL,
  layman       TEXT,
  kind         TEXT NOT NULL CHECK (kind IN ()") + QString::fromLatin1(kKindCheck) + QStringLiteral(R"()),
  source       TEXT NOT NULL,
  priority     INTEGER CHECK (priority IS NULL OR priority BETWEEN 1 AND 5),
  visibility   TEXT NOT NULL DEFAULT 'public'
                 CHECK (visibility IN ('public','internal')),
  milestone    TEXT,
  resolution   TEXT,
  body         TEXT,
  created      TEXT CHECK (created       IS NULL OR created       GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  last_modified TEXT CHECK (last_modified IS NULL OR last_modified GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  shipped      TEXT CHECK (shipped       IS NULL OR shipped       GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
  lanes        TEXT NOT NULL DEFAULT '[]',
  evidence     TEXT NOT NULL DEFAULT '[]',
  extras       TEXT NOT NULL DEFAULT '{}',
  provenance   TEXT NOT NULL DEFAULT '{}',
  UNIQUE (project_id, id_fold)
))"),
        QStringLiteral(R"(CREATE TABLE element (
  element_id  INTEGER PRIMARY KEY,
  section_id  INTEGER NOT NULL REFERENCES section(section_id),
  position    INTEGER NOT NULL,
  kind        TEXT NOT NULL CHECK (kind IN ('item','narration','table')),
  item_pk     INTEGER REFERENCES item(item_pk),
  payload     TEXT,
  UNIQUE (section_id, position),
  CHECK ((kind = 'item') = (item_pk IS NOT NULL)
     AND (kind = 'item') = (payload IS NULL))
))"),
        QStringLiteral(R"(CREATE TABLE relationship (
  rel_id      INTEGER PRIMARY KEY,
  type        TEXT NOT NULL CHECK (type IN ('splits-from','blocked-by',
                'duplicate-of','supersedes','relates-to','specified-by')),
  src_pk      INTEGER NOT NULL REFERENCES item(item_pk),
  dst_pk      INTEGER REFERENCES item(item_pk),
  dst_project TEXT,
  dst_id_fold TEXT,
  dst_path    TEXT,
  CHECK ((dst_pk IS NOT NULL) + (dst_project IS NOT NULL) + (dst_path IS NOT NULL) = 1),
  CHECK ((dst_project IS NULL) = (dst_id_fold IS NULL)),
  CHECK (dst_pk IS NULL OR dst_pk <> src_pk)
))"),
        QStringLiteral(R"(CREATE TABLE history (
  history_id  INTEGER PRIMARY KEY,
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  changed_at  TEXT NOT NULL
                CHECK (changed_at GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z'),
  seq         INTEGER NOT NULL,
  field       TEXT NOT NULL,
  old_value   TEXT,
  new_value   TEXT,
  UNIQUE (item_pk, changed_at, seq)
))"),
        QStringLiteral(R"(CREATE TABLE feedback_ref (
  item_pk     INTEGER NOT NULL REFERENCES item(item_pk),
  file        TEXT NOT NULL,
  PRIMARY KEY (item_pk, file)
))"),
        QStringLiteral(R"(CREATE TABLE citation (
  citation_id INTEGER PRIMARY KEY,
  project_id  INTEGER NOT NULL REFERENCES project(project_id),
  item_pk     INTEGER REFERENCES item(item_pk),
  doc_path    TEXT,
  target_file TEXT NOT NULL,
  symbol      TEXT NOT NULL DEFAULT '',
  CHECK ((item_pk IS NULL) != (doc_path IS NULL))
))"),
        // Three PARTIAL unique indexes, not one over every column: SQLite
        // treats NULLs as distinct, and the CHECK guarantees all but one
        // target column is NULL — so a combined constraint would never fire.
        QStringLiteral("CREATE UNIQUE INDEX rel_item_uq  ON relationship(type, src_pk, dst_pk) WHERE dst_pk IS NOT NULL"),
        QStringLiteral("CREATE UNIQUE INDEX rel_xproj_uq ON relationship(type, src_pk, dst_project, dst_id_fold) WHERE dst_project IS NOT NULL"),
        QStringLiteral("CREATE UNIQUE INDEX rel_doc_uq   ON relationship(type, src_pk, dst_path) WHERE dst_path IS NOT NULL"),
        QStringLiteral("CREATE UNIQUE INDEX cite_item_uq ON citation(item_pk, target_file, symbol) WHERE item_pk IS NOT NULL"),
        QStringLiteral("CREATE UNIQUE INDEX cite_doc_uq  ON citation(project_id, doc_path, target_file, symbol) WHERE doc_path IS NOT NULL"),
        // INV-20, "at most one" half. Found while implementing: the spec put
        // all of INV-20 in the write path on the grounds that it compares two
        // rows, but "no item is filed twice" is a UNIQUENESS property of one
        // column and a partial index expresses it exactly. Only the "at least
        // one" half needs the write path (putItem creates the element in the
        // same transaction as the item). Note this does NOT make
        // idx_element_item redundant: this index's WHERE is on `kind`, so a
        // bare `item_pk = ?` lookup does not imply it.
        QStringLiteral("CREATE UNIQUE INDEX elem_item_uq ON element(item_pk) WHERE kind = 'item'"),
        // FK indexes. SQLite does not auto-index foreign keys. Only those
        // whose column does NOT already lead a unique index or PK.
        QStringLiteral("CREATE INDEX idx_section_parent ON section(parent_id)"),
        QStringLiteral("CREATE INDEX idx_element_item   ON element(item_pk)"),
        QStringLiteral("CREATE INDEX idx_rel_src        ON relationship(src_pk)"),
        QStringLiteral("CREATE INDEX idx_rel_dst        ON relationship(dst_pk)"),
        QStringLiteral("CREATE INDEX idx_citation_proj  ON citation(project_id)"),
    };

    for (const QString &stmt : ddl) {
        if (!exec(m_db, stmt, error)) {
            exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
            return false;
        }
    }
    if (!exec(m_db, QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion), error)) {
        exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
        return false;
    }
    return exec(m_db, QStringLiteral("COMMIT"), error);
}

std::optional<qint64> RoadmapStore::registerProject(const QString &root,
                                                    const QString &name,
                                                    const QString &exportSlug,
                                                    QString *error) {
    // INV-8. canonicalFilePath() returns EMPTY for a path that does not exist,
    // and '' under UNIQUE would fuse every missing root into one project — so
    // an uncanonicalisable root is refused rather than stored.
    const QString canonical = QFileInfo(root).canonicalFilePath();
    if (canonical.isEmpty()) {
        if (error)
            *error = QStringLiteral("root does not canonicalise: %1").arg(root);
        return std::nullopt;
    }

    // Deliberately NOT `INSERT ... RETURNING`: RETURNING landed in SQLite
    // 3.35, and this spec's floor is 3.31 (generated columns, § 2.3). Using it
    // would raise the project's floor by four releases to save one round trip.
    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral("SELECT project_id FROM project WHERE root = ?"));
    sel.addBindValue(canonical);
    if (sel.exec() && sel.next())
        return sel.value(0).toLongLong();

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO project (root, name, export_slug) VALUES (?, ?, ?)"));
    q.addBindValue(canonical);
    q.addBindValue(name);
    q.addBindValue(exportSlug);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    return q.lastInsertId().toLongLong();
}

std::optional<qint64> RoadmapStore::addSection(qint64 projectId, const QString &slug,
                                               const QString &title, int level,
                                               std::optional<qint64> parentId,
                                               QString *error) {
    // Same-project rule, write-path enforced: a SQLite CHECK may not contain a
    // subquery, so a section parented across projects cannot be refused in DDL.
    if (parentId) {
        QSqlQuery p(m_db);
        p.prepare(QStringLiteral("SELECT project_id FROM section WHERE section_id = ?"));
        p.addBindValue(*parentId);
        if (!p.exec() || !p.next() || p.value(0).toLongLong() != projectId) {
            if (error)
                *error = QStringLiteral("parent section is not in project %1").arg(projectId);
            return std::nullopt;
        }
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO section (project_id, slug, title, level, parent_id) "
        "VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(projectId);
    q.addBindValue(slug);
    q.addBindValue(title);
    q.addBindValue(level);
    parentId ? q.addBindValue(*parentId) : q.addBindValue(QVariant(QMetaType(QMetaType::LongLong)));
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    return q.lastInsertId().toLongLong();
}

std::optional<qint64> RoadmapStore::putItem(const ItemWrite &w, QString *error) {
    if (!exec(m_db, QStringLiteral("BEGIN IMMEDIATE"), error))
        return std::nullopt;

    // Same-project rule (write-path, as above): the section an item is filed
    // in must belong to the item's own project — "Items are never global".
    // Read INSIDE the transaction: checking first and inserting afterwards is
    // a read-then-write race, and BEGIN IMMEDIATE already holds the write lock
    // here, so the check and the insert see one consistent snapshot.
    {
        QSqlQuery s(m_db);
        s.prepare(QStringLiteral("SELECT project_id FROM section WHERE section_id = ?"));
        s.addBindValue(w.sectionId);
        if (!s.exec() || !s.next() || s.value(0).toLongLong() != w.projectId) {
            if (error)
                *error = QStringLiteral("section %1 is not in project %2")
                             .arg(w.sectionId).arg(w.projectId);
            exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
            return std::nullopt;
        }
    }

    const auto bindOpt = [](QSqlQuery &q, const QString &v) {
        v.isEmpty() ? q.addBindValue(QVariant(QMetaType(QMetaType::QString)))
                    : q.addBindValue(v);
    };

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO item (project_id, id, id_origin, status, headline, layman, kind, "
        " source, priority, visibility, milestone, resolution, body, created, "
        " last_modified, shipped, provenance) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(w.projectId);
    q.addBindValue(w.id);
    q.addBindValue(w.idOrigin);
    q.addBindValue(w.status);
    q.addBindValue(w.headline);
    bindOpt(q, w.layman);
    q.addBindValue(w.kind);
    q.addBindValue(w.source);
    w.priority ? q.addBindValue(*w.priority) : q.addBindValue(QVariant(QMetaType(QMetaType::Int)));
    q.addBindValue(w.visibility);
    bindOpt(q, w.milestone);
    bindOpt(q, w.resolution);
    bindOpt(q, w.body);
    bindOpt(q, w.created);
    bindOpt(q, w.lastModified);
    bindOpt(q, w.shipped);
    q.addBindValue(canonicalJson(w.provenance));
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
        return std::nullopt;
    }
    const qint64 itemPk = q.lastInsertId().toLongLong();

    // INV-20 — exactly one kind='item' element per item, created here so an
    // item can never exist unfiled. The element IS the filing; there is no
    // item.section column to disagree with it.
    QSqlQuery e(m_db);
    e.prepare(QStringLiteral(
        "INSERT INTO element (section_id, position, kind, item_pk) VALUES (?,?, 'item', ?)"));
    e.addBindValue(w.sectionId);
    e.addBindValue(w.position);
    e.addBindValue(itemPk);
    if (!e.exec()) {
        if (error)
            *error = lastErr(e);
        exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
        return std::nullopt;
    }

    if (!exec(m_db, QStringLiteral("COMMIT"), error))
        return std::nullopt;
    return itemPk;
}

bool RoadmapStore::setItemField(qint64 itemPk, const QString &field,
                                const QString &value, QString *error) {
    static const QStringList writable = {
        QStringLiteral("headline"), QStringLiteral("layman"), QStringLiteral("status"),
        QStringLiteral("kind"),     QStringLiteral("source"), QStringLiteral("resolution"),
        QStringLiteral("body"),     QStringLiteral("milestone"),
        QStringLiteral("visibility"), QStringLiteral("last_modified"),
        QStringLiteral("shipped"),  QStringLiteral("created"),
    };
    if (!writable.contains(field)) {
        if (error)
            *error = QStringLiteral("field not writable: %1").arg(field);
        return false;
    }

    QSqlQuery cur(m_db);
    cur.prepare(QStringLiteral("SELECT provenance FROM item WHERE item_pk = ?"));
    cur.addBindValue(itemPk);
    if (!cur.exec() || !cur.next()) {
        if (error)
            *error = QStringLiteral("no such item %1").arg(itemPk);
        return false;
    }

    // INV-10 — per FIELD, both directions: this key becomes `asserted` and
    // every other key is carried through untouched.
    QJsonObject prov =
        QJsonDocument::fromJson(cur.value(0).toString().toUtf8()).object();
    prov.insert(field, QStringLiteral("asserted"));

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE item SET %1 = ?, provenance = ? WHERE item_pk = ?")
                  .arg(field));
    q.addBindValue(value);
    q.addBindValue(canonicalJson(prov));
    q.addBindValue(itemPk);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::relateItems(const QString &type, qint64 srcPk, qint64 dstPk,
                               QString *error) {
    qint64 a = srcPk, b = dstPk;

    if (type == QLatin1String("relates-to")) {
        // Symmetric: stored once, normalised on STABLE identity — the endpoint
        // whose (export_slug, id_fold) sorts first becomes src_pk. Normalising
        // on item_pk would be wrong: rowids are reassigned by a rebuild, so a
        // pair normalised 5->9 here can normalise 2->3 there, flipping the
        // exported direction and failing INV-1.
        const auto key = [&](qint64 pk) {
            QSqlQuery q(m_db);
            q.prepare(QStringLiteral(
                "SELECT p.export_slug || '\x1f' || i.id_fold FROM item i "
                "JOIN project p ON p.project_id = i.project_id WHERE i.item_pk = ?"));
            q.addBindValue(pk);
            return (q.exec() && q.next()) ? q.value(0).toString() : QString();
        };
        const QString ka = key(a), kb = key(b);
        if (ka.isEmpty() || kb.isEmpty()) {
            if (error)
                *error = QStringLiteral("relates-to endpoint does not resolve");
            return false;
        }
        if (kb < ka)
            std::swap(a, b);

        // Already stored in either direction? Symmetric means one row.
        QSqlQuery dup(m_db);
        dup.prepare(QStringLiteral(
            "SELECT 1 FROM relationship WHERE type = 'relates-to' "
            "AND ((src_pk = ? AND dst_pk = ?) OR (src_pk = ? AND dst_pk = ?))"));
        dup.addBindValue(a); dup.addBindValue(b);
        dup.addBindValue(b); dup.addBindValue(a);
        if (dup.exec() && dup.next())
            return true;
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO relationship (type, src_pk, dst_pk) VALUES (?,?,?)"));
    q.addBindValue(type);
    q.addBindValue(a);
    q.addBindValue(b);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::relateCrossProject(const QString &type, qint64 srcPk,
                                      const QString &dstProject,
                                      const QString &dstIdFold, QString *error) {
    // The far endpoint has no row here, so it cannot be src_pk however the pair
    // would sort (src_pk is NOT NULL REFERENCES item). The LOCAL item is always
    // src_pk, and the edge is never re-normalised if the far project later
    // arrives — a rebuild that can suddenly see one more project must not
    // silently flip a stored direction.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO relationship (type, src_pk, dst_project, dst_id_fold) "
        "VALUES (?,?,?,?)"));
    q.addBindValue(type);
    q.addBindValue(srcPk);
    q.addBindValue(dstProject);
    q.addBindValue(dstIdFold.toLower());
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

qint64 RoadmapStore::historyBytes() const {
    // The measure is pinned to length() over the three text columns rather than
    // dbstat, which needs SQLITE_ENABLE_DBSTAT_VTAB — a build flag, and the
    // schema deliberately depends on no build flags.
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    if (q.exec(QStringLiteral(
            "SELECT COALESCE(SUM(length(field) + length(coalesce(old_value,'')) "
            "+ length(coalesce(new_value,''))), 0) FROM history")) &&
        q.next())
        return q.value(0).toLongLong();
    return 0;
}

bool RoadmapStore::appendHistory(qint64 itemPk, const QString &changedAt, int seq,
                                 const QString &field, const QString &oldValue,
                                 const QString &newValue, QString *error) {
    // Below the bound nothing is ever evicted. AT the bound the write fails and
    // reports — never retried silently, never dropped.
    const qint64 incoming = field.size() + oldValue.size() + newValue.size();
    if (historyBytes() + incoming > m_historyCap) {
        if (error)
            *error = QStringLiteral("history cap reached (%1 bytes); revision refused")
                         .arg(m_historyCap);
        return false;
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO history (item_pk, changed_at, seq, field, old_value, new_value) "
        "VALUES (?,?,?,?,?,?)"));
    q.addBindValue(itemPk);
    q.addBindValue(changedAt);
    q.addBindValue(seq);
    q.addBindValue(field);
    oldValue.isNull() ? q.addBindValue(QVariant(QMetaType(QMetaType::QString)))
                      : q.addBindValue(oldValue);
    newValue.isNull() ? q.addBindValue(QVariant(QMetaType(QMetaType::QString)))
                      : q.addBindValue(newValue);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}
