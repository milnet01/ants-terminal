// ANTS-3756 — roadmap store implementation.
// Spec: docs/specs/ANTS-3756-roadmap-store-schema.md
#include "roadmapstore.h"
#include "jsoncanonical.h"
#include "secureio.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>
#include <QVariant>

#include <algorithm>
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

// The item columns a field-at-a-time write may target. File-scope rather than
// a static inside setItemField() because ANTS-3765's clearItemField() gates on
// the same list, and two copies of an allowlist is one allowlist that will
// disagree with itself.
bool isWritableItemField(const QString &field) {
    static const QStringList writable = {
        QStringLiteral("headline"), QStringLiteral("layman"), QStringLiteral("status"),
        QStringLiteral("kind"),     QStringLiteral("source"), QStringLiteral("resolution"),
        QStringLiteral("body"),     QStringLiteral("milestone"),
        QStringLiteral("visibility"), QStringLiteral("last_modified"),
        QStringLiteral("shipped"),  QStringLiteral("created"),
        // ANTS-3767 — the three JSON columns. They take a JSON TEXT value, not
        // prose, so they are validated and canonicalised before the UPDATE;
        // everything else in this list is stored as given.
        QStringLiteral("lanes"), QStringLiteral("evidence"), QStringLiteral("extras"),
    };
    return writable.contains(field);
}

} // namespace

RoadmapStore::RoadmapStore(QString dbPath, qint64 historyCapBytes, Access access)
    : m_path(dbPath.isEmpty() ? defaultPath() : std::move(dbPath)),
      m_historyCap(historyCapBytes), m_access(access) {
    // One QSqlDatabase connection name per instance: two stores in one process
    // (the tests open several) must not share a connection.
    static std::atomic<quint64> counter{0};
    m_connName = QStringLiteral("ants_roadmapstore_%1").arg(counter.fetch_add(1));
}

RoadmapStore::~RoadmapStore() {
    if (m_db.isOpen()) {
        // Refresh the planner's stale statistics on the way out. Cheap by
        // design — it ANALYZEs only what this connection actually touched —
        // and it is the difference between a query plan chosen from real row
        // counts and one chosen from none at all.
        QSqlQuery(m_db).exec(QStringLiteral("PRAGMA optimize"));
        m_db.close();
    }
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

QString RoadmapStore::canonicalJson(const QJsonValue &o) {
    // § 2.3 requires the JSON columns to be held in RFC 8785 canonical form,
    // so the export can copy those bytes rather than transform them. This used
    // to be QJsonDocument(o).toJson(Compact) on the reasoning that sorted keys
    // plus compact output IS JCS for the shapes the store writes — true for
    // every shape except one: `extras` is free-form (roadmap-data-model.md
    // § 7.7) and can hold a double, and ANTS-3761 § 2.2 measured Qt against
    // the RFC's own number table at 21 of 24. A store holding non-canonical
    // bytes breaks INV-1 at the far end of the round-trip, where it is hardest
    // to attribute.
    QByteArray out;
    if (!JsonCanonical::serialise(o, &out))
        return QString();  // callers write this into a NOT NULL column; an
                           // empty string fails the insert rather than
                           // silently storing non-canonical bytes
    return QString::fromUtf8(out);
}

bool RoadmapStore::applyPragmas(QString *error) {
    // Applied on EVERY connection at open, not once at creation.
    // foreign_keys is per-connection and defaults OFF, so without it every
    // REFERENCES in the schema is decorative.
    // busy_timeout goes first so nothing that can block runs before the
    // deadline is set. It restates rather than establishes it: Qt's QSQLITE
    // plugin already calls sqlite3_busy_timeout with a 5000 ms default of its
    // own (the QSQLITE_BUSY_TIMEOUT connect option) — measured, by dropping
    // this line and reading the pragma back. It stays because a durability
    // contract should not rest on an undocumented driver default a Qt upgrade
    // can change underneath it.
    //
    // Everything here is CONNECTION-scoped and must be re-issued on every
    // open. journal_size_limit is on this list because it was measured to be,
    // not because it reads that way: setting it, reconnecting and reading it
    // back returns -1. That is worth stating because the obvious classification
    // is wrong in the same direction on a sibling project, where it was moved
    // to a once-per-boot init path and is consequently not in force on the
    // connections that actually serve requests.
    QStringList pragmas{
        QStringLiteral("PRAGMA busy_timeout = %1")
            .arg(m_access == Access::Bulk ? kBulkBusyTimeoutMs : kBusyTimeoutMs),
        QStringLiteral("PRAGMA foreign_keys = ON"),   // per-connection; OFF by default
        QStringLiteral("PRAGMA synchronous  = FULL"), // primary store, not a cache
        // Bounds the WAL rather than the store: without it one large
        // transaction leaves a WAL that never shrinks back.
        QStringLiteral("PRAGMA journal_size_limit = %1").arg(kJournalSizeLimitBytes),
    };
    if (m_access == Access::Bulk) {
        // Only the bulk profile. A larger page cache is real resident memory,
        // and ANTS-3761's INV-12 budgets the export a peak-RSS delta under
        // 4 MiB — so the interactive/export profile stays on SQLite's 2 MiB
        // default and migration, which has no such budget, gets the cache.
        pragmas << QStringLiteral("PRAGMA cache_size = -%1").arg(kBulkCacheKiB);
    }
    // Deliberately NOT set, either profile:
    //   mmap_size   — maps the store into the address space, which makes the
    //                 export's own reads RESIDENT and breaks INV-12's delta
    //                 measurement for reasons unrelated to streaming.
    //   temp_store  — MEMORY would build a sort's temp b-tree in RAM, and
    //                 `history` is bounded at 250 MiB. Spilling to disk is the
    //                 safer failure.
    // Both are standard performance pragmas elsewhere; here they trade against
    // a stated memory budget and lose.
    for (const QString &p : pragmas) {
        QSqlQuery q(m_db);
        if (!q.exec(p)) {
            if (error)
                *error = lastErr(q) + QStringLiteral(" [") + p + QStringLiteral("]");
            return false;
        }
    }
    return enableWal(error);
}

bool RoadmapStore::enableWal(QString *error) {
    // The one statement busy_timeout does NOT cover. Switching to WAL takes an
    // EXCLUSIVE lock, and SQLite acquires it below the busy handler, so a
    // second instance opening the same store gets an immediate SQLITE_BUSY no
    // matter what the deadline says — verified, not reasoned: INV-15's forked
    // openers failed here on 18 of 25 runs with "database is locked" while
    // busy_timeout was already 5000.
    //
    // Only CREATION contends, and that is worth stating because the obvious
    // guard is wrong. A read-before-write ("skip the pragma if the mode is
    // already wal") was tried and removed: SQLite takes the exclusive lock
    // only when the mode actually CHANGES, so on an existing WAL store the
    // pragma is already a no-op and the guard changed no observable behaviour
    // — measured, by deleting it and re-running the contention test, which
    // stayed green. What actually stopped every open queueing behind a writer
    // was createSchema()'s user_version fast path, not anything here.
    QElapsedTimer clock;
    clock.start();
    QString last;
    for (;;) {
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral("PRAGMA journal_mode = WAL")) && q.next() &&
            q.value(0).toString().compare(QLatin1String("wal"), Qt::CaseInsensitive) == 0)
            return true;
        // A successful exec reporting a mode other than WAL is a failure too:
        // SQLite answers with the mode still in force, not with an error.
        last = q.lastError().text();
        if (clock.elapsed() >= kBusyTimeoutMs)
            break;
        QThread::msleep(10);
    }
    if (error)
        *error = last + QStringLiteral(" [PRAGMA journal_mode = WAL, after %1 ms]")
                            .arg(kBusyTimeoutMs);
    return false;
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

// ANTS-3781 § 2.1 — the production ladder. INV-4 is what makes a bump without a
// rung a red test rather than a thing to remember.
//
// A function-local static, so it is built once on first call.
const QVector<RoadmapStore::Upgrade> &RoadmapStore::upgradeLadder() {
    static const QVector<Upgrade> ladder = {
        // ANTS-3815 § 2.2 — the first rung. Written FROM the diff createSchema()'s
        // DDL took, not from the shape intended (ANTS-3781 § 2.1's obligation on
        // whoever bumps); ANTS-3815 INV-3 is what proves the two agree.
        //
        // DEFAULT '' is load-bearing, not decoration: SQLite refuses
        // `ADD COLUMN … NOT NULL` with no default on a table that HAS ROWS and
        // accepts it on an empty one (measured, 3.53.2). A defaultless rung
        // would therefore pass against every empty test store and fail against
        // every real one — the worst available shape for a migration.
        Upgrade{2, {QStringLiteral(
            "ALTER TABLE project ADD COLUMN source_format TEXT NOT NULL DEFAULT '' "
            "CHECK (source_format IN ('', 'ants-v1', 'github-task-list', 'pass-headings'))")}},
    };
    return ladder;
}

// ANTS-3781 § 2.1 / INV-1..INV-3, INV-7. Runs entirely inside the CALLER's
// transaction (see the header's precondition), so this function opens none of
// its own and the stamp below is durable only once the caller commits.
bool RoadmapStore::applyUpgrades(QSqlDatabase &db, int from, int to,
                                 const QVector<Upgrade> &ladder, QString *error) {
    // Degenerate rule 1, and it is FIRST because rule 2 overlaps it: version 0
    // is a store with no schema, which the DDL creates rather than a rung, and
    // a negative value is a corrupt pragma. createSchema() routes every
    // non-zero version here precisely so this arm can refuse legibly instead of
    // the DDL answering with `table already exists`.
    if (from < 1) {
        if (error)
            *error = QStringLiteral(
                         "cannot upgrade roadmap store schema %1 to %2: version 0 is a "
                         "store with no schema, which is the DDL's case, and a negative "
                         "version is a corrupt user_version pragma")
                         .arg(from)
                         .arg(to);
        return false;
    }
    // Degenerate rule 2 — an empty climb. A downgrade range lands here as a
    // silent no-op by design; refusing a downgrade is createSchema()'s
    // `version > kSchemaVersion` arm, two branches above this call.
    if (from >= to)
        return true;

    // PASS ONE — validate the WHOLE range and execute nothing (INV-2). A single
    // pass that looked each rung up as it reached it would run rung 2's
    // statements before discovering rung 3 was missing, leaving a half-climbed
    // store for the caller to roll back.
    for (int v = from + 1; v <= to; ++v) {
        int rungs = 0;
        for (const Upgrade &u : ladder)
            if (u.to == v)
                ++rungs;
        if (rungs != 1) {
            if (error)
                *error = QStringLiteral(
                             "cannot upgrade roadmap store schema %1 to %2: version %3 has "
                             "%4 upgrade rungs, expected exactly one")
                             .arg(from)
                             .arg(to)
                             .arg(v)
                             .arg(rungs);
            return false;
        }
    }

    // PASS TWO — ascending VERSION order, not declaration order (INV-1).
    for (int v = from + 1; v <= to; ++v) {
        for (const Upgrade &u : ladder) {
            if (u.to != v)
                continue;
            for (const QString &stmt : u.statements) {
                QString why;
                if (!exec(db, stmt, &why)) {
                    if (error)
                        *error = QStringLiteral("cannot upgrade roadmap store schema %1 to "
                                                "%2: the rung to version %3 failed: %4")
                                     .arg(from)
                                     .arg(to)
                                     .arg(v)
                                     .arg(why);
                    return false;
                }
            }
        }
    }

    // One stamp, after the last rung — never per rung, so the version moves
    // only once and only if every rung landed (INV-1).
    QString why;
    if (!exec(db, QStringLiteral("PRAGMA user_version = %1").arg(to), &why)) {
        if (error)
            *error = QStringLiteral("cannot upgrade roadmap store schema %1 to %2: "
                                    "stamping user_version failed: %3")
                         .arg(from)
                         .arg(to)
                         .arg(why);
        return false;
    }
    return true;
}

bool RoadmapStore::createSchema(QString *error) {
    // Fast path, and it is about CONTENTION rather than speed: on an existing
    // store there is nothing to create, so taking a write lock to discover
    // that makes every ordinary open queue behind any active writer. Measured
    // — opening while another connection held a write transaction took the
    // full 5000 ms deadline before this check existed. Reading user_version
    // needs only a shared lock, which WAL grants alongside a writer.
    //
    // This is an optimisation, not the discriminator: the authoritative read
    // is still the one inside BEGIN IMMEDIATE below, so two processes racing
    // to create a store are decided there exactly as before.
    {
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next() &&
            q.value(0).toInt() == kSchemaVersion)
            return true;
    }

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

    // ANTS-3781 § 2.2 — everything below kSchemaVersion and not 0 comes here,
    // negatives included (the two arms above already returned). The guard is
    // `!= 0` and not `> 0` deliberately: user_version is a SIGNED 32-bit
    // pragma, so a negative value is representable, and under `> 0` it would
    // fall through to the DDL and be answered with `table already exists` — the
    // illegible failure this item exists to close, one input away from the case
    // everyone thinks about. applyUpgrades()'s `from < 1` arm refuses it naming
    // the value instead.
    //
    // The version read here is the one read inside BEGIN IMMEDIATE, so two
    // binaries opening the same behind-version store serialise: the winner
    // climbs and commits, and the loser's own read returns kSchemaVersion and
    // takes the arm above. m_createdSchema is deliberately NOT set — an
    // upgraded store's tables were made by an earlier binary (INV-5).
    if (version != 0) {
        if (!applyUpgrades(m_db, version, kSchemaVersion, upgradeLadder(), error)) {
            exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
            return false;
        }
        return exec(m_db, QStringLiteral("COMMIT"), error);
    }

    // ANTS-3815 § 2.1 — `project.source_format` records which roadmap dialect the
    // migration read this project's LIVE roadmap (source index 0) in. '' means
    // "not recorded" and is not a format: it is what a version-1 row takes when
    // the rung runs, because a rung is SQL and cannot open a roadmap to classify
    // it. § 2.4 makes that the "behave exactly as version 1 did" path.
    //
    // APPENDED LAST in the CREATE TABLE, and that is a rule rather than a layout
    // preference: ALTER TABLE ADD COLUMN can only append, so the climbed store's
    // column order is fixed for it and a DDL grouping this beside `root` would
    // build a different table. INV-3 is what reds it. Every future column
    // inherits the constraint.
    //
    // THE TEXT BELOW IS PART OF THE SCHEMA. SQLite stores a CREATE TABLE
    // verbatim, comments included, and an ALTER splices in only the column
    // definition it is handed. So two rules follow, both discovered by INV-3
    // reddening on the first run of this bump, which is exactly the disagreement
    // it exists to find:
    //
    //   1. A new column's rationale goes HERE, above the array — not as a `--`
    //      comment beside it, the way `section.source_path` below could still
    //      afford at version 1. Such a comment would exist in a DDL-built store
    //      and not in a climbed one, and the two could never compare equal under
    //      any normalisation.
    //   2. No EXISTING character of a shipped CREATE TABLE may change — comments
    //      included. The ONLY permitted edit is appending a new column
    //      definition last, which is exactly what a rung reproduces and
    //      therefore the one edit the two stores can agree on. Stated that way
    //      round deliberately: read as a blanket "this text can never change",
    //      the rule would forbid the very append a bump is here to make. A
    //      climbed store replays the frozen text of the version that created it,
    //      so editing a word *inside* one forks the two stores permanently for
    //      no schema benefit.
    //
    // Rule 2 has one live consequence. `section.source_path`'s comment below says
    // a later column is "an ALTER in a rung RATHER THAN an edit here". That is
    // wrong — ANTS-3781 § 2.1 requires a bump to be BOTH, as two expressions of
    // one change, and ANTS-3815 is the first to make them. ANTS-3815 § 8
    // scheduled that sentence for correction in place; it cannot be corrected in
    // place, because it lives inside SQL that shipped at version 1. This
    // paragraph is the correction.
    const QString ddl[] = {
        QStringLiteral(R"(CREATE TABLE project (
  project_id   INTEGER PRIMARY KEY,
  root         TEXT UNIQUE,
  name         TEXT NOT NULL,
  export_slug  TEXT NOT NULL UNIQUE
                 CHECK (export_slug GLOB '[a-z0-9]*'
                    AND export_slug NOT GLOB '*[^a-z0-9-]*'),
  legend       TEXT NOT NULL DEFAULT '{}',
  source_format TEXT NOT NULL DEFAULT ''
                 CHECK (source_format IN ('', 'ants-v1', 'github-task-list', 'pass-headings'))
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
  -- ANTS-3782 § 2.1. Which source file this section was read from,
  -- project-root-relative; NULL is the live roadmap. It is the only record of
  -- that fact -- the migration plan holds a source index and is discarded at
  -- commit -- so without it ANTS-3758 re-emits a rotated archive back into
  -- ROADMAP.md. In this DDL rather than an ALTER, and at user_version 1: no
  -- store is reachable from user-facing code yet, so there is nothing to
  -- migrate and a bump would manufacture an upgrade case in order to migrate
  -- zero stores. That freedom expires at ANTS-3758's cutover. ANTS-3781 has
  -- since built what follows -- applyUpgrades() above -- so a later column is
  -- an ALTER in a rung rather than an edit here, and it no longer touches the
  -- export goldens: the export carries its own record-version constant now.
  source_path TEXT,
  -- ANTS-3796 § 2.1. Document order among THIS PROJECT's sections -- one
  -- sequence over every section, not one per parent -- and the only record of
  -- it: the migration plan knows it while it runs (sourceIndex, firstLine) and
  -- discards it at commit, exactly as it did source_path above. Without it
  -- siblings are ordered only by the section_id surrogate, which a rebuild
  -- reassigns in (depth, slug) order, so ANTS-3758's render would re-file this
  -- project's own prose sections among its version numbers on the first
  -- recovery from backup.
  --
  -- Project-wide rather than per-parent so that parents-before-children falls
  -- out of the data (a heading precedes its subheadings in the file it was read
  -- from) instead of needing UNIQUE (parent_id, position), which SQLite would
  -- not bind anyway: it treats NULLs as distinct, so the top-level siblings
  -- that actually reorder would be unconstrained.
  --
  -- Deliberately NOT UNIQUE (project_id, position), which reads like the
  -- obvious constraint. `element` gets away with UNIQUE (section_id, position)
  -- only because clearSectionElements() lets the migration delete and rewrite
  -- the whole sequence; sections cannot be cleared (element.section_id and item
  -- filing reference them), so a re-run swapping two sections would collide
  -- mid-update with no escape -- SQLite offers DEFERRABLE INITIALLY DEFERRED
  -- for foreign keys only. Distinctness is a writer's obligation within a run's
  -- plan; sectionOrderLess()'s (position, slug) key is total regardless.
  --
  -- NOT NULL with no default, so every writer states it rather than silently
  -- taking 0. At user_version 1 for the reason source_path above records, and
  -- this is the LAST change entitled to that freedom: it expires at ANTS-3758's
  -- cutover.
  position    INTEGER NOT NULL,
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
    if (!exec(m_db, QStringLiteral("COMMIT"), error))
        return false;
    // INV-15 — this connection is the creator. Only reachable through the
    // version == 0 branch, so exactly one racing opener can ever set it.
    m_createdSchema = true;
    return true;
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
                                               int position,
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
        "INSERT INTO section (project_id, slug, title, level, position, parent_id) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(projectId);
    q.addBindValue(slug);
    q.addBindValue(title);
    q.addBindValue(level);
    q.addBindValue(position);
    parentId ? q.addBindValue(*parentId) : q.addBindValue(QVariant(QMetaType(QMetaType::LongLong)));
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    return q.lastInsertId().toLongLong();
}

std::optional<qint64> RoadmapStore::putItem(const ItemWrite &w, QString *error) {
    // ANTS-3765 § 2.3 / INV-8 — atomic either way. With no transaction open it
    // opens and commits its own, because INV-20 depends on the item and its
    // element being written together and every existing caller relies on it.
    // With one open it PARTICIPATES, and INV-20 holds a fortiori: the
    // enclosing transaction is wider, not narrower.
    //
    // The failure paths below roll back only when this call owns the
    // transaction. Rolling back one it does not own is the sharpest edge in
    // the change: the caller would carry on believing it was still in a
    // transaction, every later write would autocommit and PERSIST, and the
    // migration would report a clean partial load — INV-1 inverted with
    // nothing observing it.
    const bool owns = !m_inTransaction;
    if (owns && !exec(m_db, QStringLiteral("BEGIN IMMEDIATE"), error))
        return std::nullopt;
    const auto unwind = [&] {
        if (owns)
            exec(m_db, QStringLiteral("ROLLBACK"), nullptr);
    };

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
            unwind();
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
        " last_modified, shipped, lanes, evidence, extras, provenance) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
    // ANTS-3767 — canonical at the write path, so the export copies these
    // bytes rather than transforming them (§ 2.3). An empty list still writes
    // '[]' / '{}' explicitly rather than leaning on the DDL default: one
    // producer of these columns, not two.
    q.addBindValue(canonicalJson(QJsonArray::fromStringList(w.lanes)));
    q.addBindValue(canonicalJson(QJsonArray::fromStringList(w.evidence)));
    q.addBindValue(canonicalJson(w.extras));
    q.addBindValue(canonicalJson(w.provenance));
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        unwind();
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
        unwind();
        return std::nullopt;
    }

    if (owns && !exec(m_db, QStringLiteral("COMMIT"), error))
        return std::nullopt;
    return itemPk;
}

bool RoadmapStore::setItemField(qint64 itemPk, const QString &field,
                                const QString &value, QString *error) {
    // The 3-argument form IS the human edit, so `asserted` is its meaning and
    // stays hardcoded here rather than becoming a default argument — a default
    // would make every migration write that forgot the parameter silently
    // claim a human asserted it.
    return setItemField(itemPk, field, value, QStringLiteral("asserted"), error);
}

bool RoadmapStore::setItemField(qint64 itemPk, const QString &field,
                                const QString &value, const QString &provenance,
                                QString *error) {
    if (!isWritableItemField(field)) {
        if (error)
            *error = QStringLiteral("field not writable: %1").arg(field);
        return false;
    }

    // Shape, not merely parseability: `{"a":1}` is valid JSON and is not a lane
    // list. A parse-only guard puts an object in an array column, which nothing
    // downstream — the export included — expects to have to cope with.
    QString stored = value;
    const bool isList = field == QLatin1String("lanes") || field == QLatin1String("evidence");
    if (isList || field == QLatin1String("extras")) {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(value.toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError) {
            if (error)
                *error = QStringLiteral("%1: not JSON: %2").arg(field, pe.errorString());
            return false;
        }
        if (isList) {
            if (!doc.isArray()) {
                if (error)
                    *error = QStringLiteral("%1 must be a JSON array of strings").arg(field);
                return false;
            }
            for (const QJsonValue &v : doc.array()) {
                if (!v.isString()) {
                    if (error)
                        *error = QStringLiteral("%1 must be a JSON array of strings").arg(field);
                    return false;
                }
            }
            stored = canonicalJson(doc.array());
        } else {
            if (!doc.isObject()) {
                if (error)
                    *error = QStringLiteral("extras must be a JSON object");
                return false;
            }
            stored = canonicalJson(doc.object());
        }
        if (stored.isEmpty()) {
            // canonicalJson() returns empty rather than storing non-canonical
            // bytes — a NOT NULL column would take '' happily, so refuse here.
            if (error)
                *error = QStringLiteral("%1: value cannot be canonicalised").arg(field);
            return false;
        }
    }

    QSqlQuery cur(m_db);
    cur.prepare(QStringLiteral("SELECT provenance FROM item WHERE item_pk = ?"));
    cur.addBindValue(itemPk);
    if (!cur.exec() || !cur.next()) {
        if (error)
            *error = QStringLiteral("no such item %1").arg(itemPk);
        return false;
    }

    // INV-10 — per FIELD, both directions: this key takes the caller's
    // provenance and every other key is carried through untouched. That
    // carry-through is also ANTS-3765 § 2.6's "provenance is merged, not
    // replaced" — a field a re-run did not write keeps the provenance it had.
    QJsonObject prov =
        QJsonDocument::fromJson(cur.value(0).toString().toUtf8()).object();
    prov.insert(field, provenance);

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE item SET %1 = ?, provenance = ? WHERE item_pk = ?")
                  .arg(field));
    q.addBindValue(stored);
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
    //
    // Through historyWouldExceedCap() rather than inline since ANTS-3822: that
    // accessor IS this comparison, and two other callers ask it in advance. A
    // second spelling here is how the in-advance answer and the actual refusal
    // drift apart.
    const qint64 incoming = field.size() + oldValue.size() + newValue.size();
    if (historyWouldExceedCap(incoming)) {
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

// --- ANTS-3765 § 2.3 — explicit transaction control --------------------------

bool RoadmapStore::begin(QString *error) {
    // INV-7 — refuses to nest rather than no-oping. SQLite refuses the nested
    // BEGIN itself; a no-op here would be this project's own invention layered
    // over that refusal, and it would make a caller's commit() end a
    // transaction it did not open.
    if (m_inTransaction) {
        if (error)
            *error = QStringLiteral("a transaction is already open");
        return false;
    }
    if (!exec(m_db, QStringLiteral("BEGIN IMMEDIATE"), error))
        return false;
    m_inTransaction = true;
    return true;
}

bool RoadmapStore::commit(QString *error) {
    // Refusing when none is open rather than returning success: a silent no-op
    // is the same class of defect as a nesting begin() — the caller believes it
    // has ended a transaction and has not.
    if (!m_inTransaction) {
        if (error)
            *error = QStringLiteral("no transaction is open");
        return false;
    }
    // The flag clears only on success: a COMMIT refused as SQLITE_BUSY leaves
    // the transaction active, and clearing here would strand it open with
    // nothing believing it owns it.
    if (!exec(m_db, QStringLiteral("COMMIT"), error))
        return false;
    m_inTransaction = false;
    return true;
}

bool RoadmapStore::rollback(QString *error) {
    if (!m_inTransaction) {
        if (error)
            *error = QStringLiteral("no transaction is open");
        return false;
    }
    // Unconditionally clears, unlike commit(): the guard above has already
    // established a transaction was open, so the only reachable failure is one
    // SQLite has itself already aborted the transaction for. Leaving the flag
    // set there would deny the caller any way back to a usable connection.
    const bool ok = exec(m_db, QStringLiteral("ROLLBACK"), error);
    m_inTransaction = false;
    return ok;
}

// --- ANTS-3765 § 2.4 — writers -----------------------------------------------

// ANTS-3782 § 2.2. Note the deliberate DIFFERENCE from setSectionIntro()
// below: an engaged optional holding an empty string is bound as that empty
// string, NOT folded to NULL. '' is a meaningless intro and a wrong source
// path — folding it would make an unplaceable value read back as the live
// roadmap, collapsing the distinction std::optional is here to carry.
bool RoadmapStore::setSectionSource(qint64 sectionId,
                                    const std::optional<QString> &sourcePath,
                                    QString *error) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE section SET source_path = ? WHERE section_id = ?"));
    if (sourcePath) {
        // A DEFAULT-CONSTRUCTED QString is null, and the SQLite driver binds a
        // null string as SQL NULL — which would fold an engaged-but-empty
        // optional back onto the live-roadmap value this method exists to keep
        // distinct, silently. Normalise it, the same way roadmapmigrateload's
        // notNull() does for every other text column.
        q.addBindValue(sourcePath->isNull() ? QString::fromUtf8("") : *sourcePath);
    } else {
        q.addBindValue(QVariant(QMetaType(QMetaType::QString)));
    }
    q.addBindValue(sectionId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

// ANTS-4070 § 2.1 — see the header for why this is not part of
// updateSection()'s tuple, and why a collision is a failure here.
bool RoadmapStore::setSectionSlug(qint64 sectionId, const QString &slug,
                                  QString *error) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE section SET slug = ? WHERE section_id = ?"));
    q.addBindValue(slug);
    q.addBindValue(sectionId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::setSectionIntro(qint64 sectionId, const QString &intro,
                                   QString *error) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE section SET intro = ? WHERE section_id = ?"));
    // Prose, stored VERBATIM — never canonicalised. An empty intro is NULL
    // rather than '', so a section that never had one and a section whose one
    // was removed are the same state (ANTS-3761's column diff sees both).
    intro.isEmpty() ? q.addBindValue(QVariant(QMetaType(QMetaType::QString)))
                    : q.addBindValue(intro);
    q.addBindValue(sectionId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::updateSection(qint64 sectionId, const QString &title, int level,
                                 int position, std::optional<qint64> parentId,
                                 QString *error) {
    // Same-project rule, write-path enforced exactly as addSection() does it: a
    // SQLite CHECK may not contain a subquery, so a section re-parented across
    // projects cannot be refused in DDL.
    if (parentId) {
        QSqlQuery own(m_db);
        own.prepare(QStringLiteral("SELECT project_id FROM section WHERE section_id = ?"));
        own.addBindValue(sectionId);
        QSqlQuery p(m_db);
        p.prepare(QStringLiteral("SELECT project_id FROM section WHERE section_id = ?"));
        p.addBindValue(*parentId);
        if (!own.exec() || !own.next() || !p.exec() || !p.next() ||
            own.value(0).toLongLong() != p.value(0).toLongLong()) {
            if (error)
                *error = QStringLiteral("parent section %1 is not in section %2's project")
                             .arg(*parentId).arg(sectionId);
            return false;
        }
        if (*parentId == sectionId) {
            if (error)
                *error = QStringLiteral("section %1 cannot be its own parent").arg(sectionId);
            return false;
        }
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE section SET title = ?, level = ?, position = ?, parent_id = ? "
        "WHERE section_id = ?"));
    q.addBindValue(title);
    q.addBindValue(level);
    q.addBindValue(position);
    parentId ? q.addBindValue(*parentId) : q.addBindValue(QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(sectionId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

// The `table` payload rule, shared by addElement() and setElementPayload().
// ANTS-3809 § 2.2 requires the second to canonicalise "exactly as addElement()
// does"; one function is what makes that literally true rather than two copies
// that agree today. Returns false with *error set; `stored` is the bytes to
// bind. Non-table kinds are VERBATIM — § 2.3 calls canonicalising prose as JSON
// undefined rather than merely wasteful, so a narration payload that happens to
// look like JSON round-trips unchanged.
static bool rsCanonicalisePayload(const QString &kind, const QString &payload,
                                  QString *stored, QString *error) {
    if (kind != QLatin1String("table")) {
        *stored = payload;
        return true;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("table payload is not JSON: %1").arg(pe.errorString());
        return false;
    }
    *stored = RoadmapStore::canonicalJson(doc.isArray() ? QJsonValue(doc.array())
                                                        : QJsonValue(doc.object()));
    if (stored->isEmpty()) {
        if (error)
            *error = QStringLiteral("table payload cannot be canonicalised");
        return false;
    }
    return true;
}

bool RoadmapStore::addElement(qint64 sectionId, int position, const QString &kind,
                              const QString &payload, QString *error) {
    // INV-10 — refused HERE, not by the DDL CHECK. This method has no item_pk
    // parameter, so it could not produce a well-formed filing even if it tried,
    // and letting the CHECK decide would report a constraint violation for
    // what is really a misuse of the API.
    if (kind == QLatin1String("item")) {
        if (error)
            *error = QStringLiteral("addElement() cannot file an item; use putItem() "
                                    "or fileItem()");
        return false;
    }

    QString stored;
    if (!rsCanonicalisePayload(kind, payload, &stored, error))
        return false;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO element (section_id, position, kind, payload) VALUES (?,?,?,?)"));
    q.addBindValue(sectionId);
    q.addBindValue(position);
    q.addBindValue(kind);
    q.addBindValue(stored);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

// ANTS-3809 § 2.2. See roadmapstore.h for the contract; the row's own kind
// drives both the refusal and the canonicalisation, so it is read first and
// nothing is written until both have passed.
bool RoadmapStore::setElementPayload(qint64 sectionId, int position,
                                     const QString &payload, QString *error) {
    QSqlQuery sel(m_db);
    sel.prepare(QStringLiteral(
        "SELECT kind FROM element WHERE section_id = ? AND position = ?"));
    sel.addBindValue(sectionId);
    sel.addBindValue(position);
    if (!sel.exec()) {
        if (error)
            *error = lastErr(sel);
        return false;
    }
    if (!sel.next()) {
        if (error)
            *error = QStringLiteral("no element at section %1 position %2")
                         .arg(sectionId).arg(position);
        return false;
    }

    const QString kind = sel.value(0).toString();
    if (kind == QLatin1String("item")) {
        if (error)
            *error = QStringLiteral("setElementPayload() cannot rewrite an item "
                                    "filing; use putItem() or fileItem()");
        return false;
    }

    QString stored;
    if (!rsCanonicalisePayload(kind, payload, &stored, error))
        return false;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE element SET payload = ? WHERE section_id = ? AND position = ?"));
    q.addBindValue(stored);
    q.addBindValue(sectionId);
    q.addBindValue(position);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::fileItem(qint64 itemPk, qint64 sectionId, int position,
                            QString *error) {
    // INV-10 — the already-filed check is here rather than left to
    // elem_item_uq, which does catch it but as a constraint violation that
    // aborts the caller's whole migration instead of a reported refusal.
    QSqlQuery cur(m_db);
    cur.prepare(QStringLiteral(
        "SELECT 1 FROM element WHERE kind = 'item' AND item_pk = ?"));
    cur.addBindValue(itemPk);
    if (!cur.exec()) {
        if (error)
            *error = lastErr(cur);
        return false;
    }
    if (cur.next()) {
        if (error)
            *error = QStringLiteral("item %1 is already filed").arg(itemPk);
        return false;
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO element (section_id, position, kind, item_pk) VALUES (?,?, 'item', ?)"));
    q.addBindValue(sectionId);
    q.addBindValue(position);
    q.addBindValue(itemPk);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::unfileItem(qint64 itemPk, QString *error) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM element WHERE kind = 'item' AND item_pk = ?"));
    q.addBindValue(itemPk);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::clearSectionElements(qint64 sectionId, QString *error) {
    // Per SECTION, never per project: the signature is the contract. A
    // project-wide delete would take out the narration and table rows of
    // sections the plan no longer carries, which nothing re-inserts.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM element WHERE section_id = ?"));
    q.addBindValue(sectionId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::setLegend(qint64 projectId, const QJsonObject &legend,
                             QString *error) {
    const QString stored = canonicalJson(legend);
    if (stored.isEmpty()) {
        if (error)
            *error = QStringLiteral("legend cannot be canonicalised");
        return false;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE project SET legend = ? WHERE project_id = ?"));
    q.addBindValue(stored);
    q.addBindValue(projectId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

// ANTS-3815 § 2.4 / INV-7. Two refusals, and the first is why this does not
// simply return exec()'s result: an UPDATE matching no row SUCCEEDS in SQLite,
// so a bare exec() reports having written a project that does not exist. That is
// the ANTS-3767 failure mode one column along — a column with a writer that
// always claims to have written. The second refusal is the DDL's CHECK, which
// SQLite raises for a format outside § 2.1's set.
bool RoadmapStore::setProjectSourceFormat(qint64 projectId, const QString &format,
                                          QString *error) {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE project SET source_format = ? WHERE project_id = ?"));
    q.addBindValue(format);
    q.addBindValue(projectId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    if (q.numRowsAffected() != 1) {
        if (error)
            *error = QStringLiteral("no project row %1 to record a source format on")
                         .arg(projectId);
        return false;
    }
    return true;
}

bool RoadmapStore::raiseIdHighWater(qint64 projectId, const QString &prefix,
                                    qint64 highWater, QString *error) {
    // Upward only, expressed in the UPSERT rather than in a read-then-write:
    // two migrations racing on one store must not be able to lower a counter
    // between the read and the write, and MAX() in the conflict clause makes a
    // value at or below the stored one a no-op rather than an error.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO id_prefix (project_id, prefix, high_water) VALUES (?,?,?) "
        "ON CONFLICT (project_id, prefix) DO UPDATE SET "
        "high_water = MAX(high_water, excluded.high_water)"));
    q.addBindValue(projectId);
    q.addBindValue(prefix);
    q.addBindValue(highWater);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

bool RoadmapStore::clearItemField(qint64 itemPk, const QString &field,
                                  const QString &provenance, QString *error) {
    if (!isWritableItemField(field)) {
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
    QJsonObject prov =
        QJsonDocument::fromJson(cur.value(0).toString().toUtf8()).object();
    prov.insert(field, provenance);

    // A NOT NULL column — status, headline, or one of the three JSON ones —
    // refuses this, and that refusal is the engine's rather than a second
    // allowlist here: there is exactly one statement of which columns may hold
    // NULL, and it is the DDL.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE item SET %1 = NULL, provenance = ? WHERE item_pk = ?")
                  .arg(field));
    q.addBindValue(canonicalJson(prov));
    q.addBindValue(itemPk);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return false;
    }
    return true;
}

// --- ANTS-3765 § 2.4 — readers -----------------------------------------------

std::optional<qint64> RoadmapStore::findItem(qint64 projectId, const QString &id,
                                             QString *error) const {
    // lower() and not QString::toLower(): id_fold is a GENERATED column over
    // SQLite's own ASCII-only lower(), and folding the probe in Qt — which is
    // Unicode-aware — would miss a row the column holds.
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT item_pk FROM item WHERE project_id = ? AND id_fold = lower(?)"));
    q.addBindValue(projectId);
    q.addBindValue(id);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;   // no such item — NOT an error; `error` stays clear
    return q.value(0).toLongLong();
}

namespace {
// The item SELECT list, and the row assembly that reads it back. Shared by the
// point lookup and the batched reader below so the two cannot drift into
// returning different ItemWrites for the same row — which would be invisible
// until something compared them.
//
// LEFT JOIN: an item is unfiled only transiently, inside § 2.6's element
// rebuild, but this reader is what captures a filing BEFORE that delete and an
// INNER JOIN would return nothing for an item read after it.
constexpr const char *kItemColumns =
    "SELECT i.project_id, i.id, i.id_origin, i.status, i.headline, i.layman, "
    "       i.kind, i.source, i.priority, i.visibility, i.milestone, "
    "       i.resolution, i.body, i.created, i.last_modified, i.shipped, "
    "       i.lanes, i.evidence, i.extras, i.provenance, "
    "       e.section_id, e.position, i.item_pk "
    "FROM item i LEFT JOIN element e "
    "  ON e.item_pk = i.item_pk AND e.kind = 'item' ";

RoadmapStore::ItemWrite itemFromRow(const QSqlQuery &q) {
    const auto strList = [](const QString &json) {
        QStringList out;
        for (const QJsonValue &v : QJsonDocument::fromJson(json.toUtf8()).array())
            out << v.toString();
        return out;
    };

    RoadmapStore::ItemWrite w;
    w.projectId = q.value(0).toLongLong();
    w.id = q.value(1).toString();
    w.idOrigin = q.value(2).toString();
    w.status = q.value(3).toString();
    w.headline = q.value(4).toString();
    w.layman = q.value(5).toString();
    w.kind = q.value(6).toString();
    w.source = q.value(7).toString();
    if (!q.value(8).isNull())
        w.priority = q.value(8).toInt();
    w.visibility = q.value(9).toString();
    w.milestone = q.value(10).toString();
    w.resolution = q.value(11).toString();
    w.body = q.value(12).toString();
    w.created = q.value(13).toString();
    w.lastModified = q.value(14).toString();
    w.shipped = q.value(15).toString();
    w.lanes = strList(q.value(16).toString());
    w.evidence = strList(q.value(17).toString());
    w.extras = QJsonDocument::fromJson(q.value(18).toString().toUtf8()).object();
    w.provenance = QJsonDocument::fromJson(q.value(19).toString().toUtf8()).object();
    w.sectionId = q.value(20).toLongLong();   // 0 when unfiled
    w.position = q.value(21).toInt();
    return w;
}
} // namespace

std::optional<RoadmapStore::ItemWrite> RoadmapStore::readItem(qint64 itemPk,
                                                              QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QLatin1String(kItemColumns) + QStringLiteral("WHERE i.item_pk = ?"));
    q.addBindValue(itemPk);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    return itemFromRow(q);
}

std::optional<QHash<qint64, RoadmapStore::ItemWrite>>
RoadmapStore::readItems(qint64 projectId, QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QLatin1String(kItemColumns) + QStringLiteral("WHERE i.project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    QHash<qint64, ItemWrite> out;
    while (q.next())
        out.insert(q.value(22).toLongLong(), itemFromRow(q));
    return out;
}

std::optional<qint64>
RoadmapStore::projectBodyBytes(qint64 projectId, QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    // CAST(... AS BLOB) is load-bearing: SQLite's LENGTH() over TEXT counts
    // CHARACTERS and counts BYTES only for a BLOB. See the header for why that
    // distinction, and the separate UTF-16 resident-size one, are called out
    // rather than left to the reader.
    //
    // COALESCE because SUM() over zero rows is NULL, not 0 — an empty project
    // would otherwise return whatever toLongLong() makes of a null QVariant,
    // which is 0 by luck rather than by contract, and indistinguishable from
    // the failure this function reports as nullopt.
    q.prepare(QStringLiteral(
        "SELECT COALESCE(SUM(LENGTH(CAST(body AS BLOB))), 0) "
        "FROM item WHERE project_id = ?"));
    q.addBindValue(projectId);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    return q.value(0).toLongLong();
}

// ---------------------------------------------------------------- ANTS-4501 --
// The report's aggregate reader. See roadmapstore.h for why this lives on the
// store rather than as raw SQL in the envelope builder.
namespace {

// scope:"all" is std::nullopt, which is one project_id predicate rather than
// two query strings. Written as a fragment + a bind rather than string
// interpolation of the id: the id is trusted here, but a reader should not have
// to establish that, and the two forms cost the same.
QString rsScopeClause(const std::optional<qint64> &projectId,
                      const QString &alias = QString()) {
    const QString col = alias.isEmpty() ? QStringLiteral("project_id")
                                        : alias + QStringLiteral(".project_id");
    return projectId ? QStringLiteral(" AND %1 = ?").arg(col) : QString();
}

void rsBindScope(QSqlQuery &q, const std::optional<qint64> &projectId) {
    if (projectId) q.addBindValue(*projectId);
}

// The open set, spelled as an enumeration and never as `status != 'shipped'`
// (ANTS-4501 § 2.5). The schema's CHECK admits a fifth value, `dropped`, and a
// dropped item is not outstanding work — the two forms disagree about it, and
// no corpus with zero dropped rows can tell them apart.
const char *kOpenStatusIn = " AND status IN ('planned','in-progress','considered')";

}  // namespace

std::optional<RoadmapStore::ReportCounts>
RoadmapStore::reportCounts(std::optional<qint64> projectId, QString *error) const {
    ReportCounts out;
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));

    // Pass 1 — status tally. `1=1` so the scope fragment can start with AND on
    // every branch; the alternative is assembling the WHERE keyword
    // conditionally, which is where this shape usually goes wrong.
    q.prepare(QStringLiteral("SELECT status, COUNT(*) FROM item WHERE 1=1%1 "
                             "GROUP BY status").arg(rsScopeClause(projectId)));
    rsBindScope(q, projectId);
    if (!q.exec()) { if (error) *error = lastErr(q); return std::nullopt; }
    while (q.next()) {
        const int n = q.value(1).toInt();
        out.byStatus.insert(q.value(0).toString(), n);
        out.items += n;                 // totals.items is the sum of by_status
    }

    // Pass 2 — kind tally.
    q.prepare(QStringLiteral("SELECT kind, COUNT(*) FROM item WHERE 1=1%1 "
                             "GROUP BY kind").arg(rsScopeClause(projectId)));
    rsBindScope(q, projectId);
    if (!q.exec()) { if (error) *error = lastErr(q); return std::nullopt; }
    while (q.next())
        out.byKind.insert(q.value(0).toString(), q.value(1).toInt());

    // Pass 3 — coverage. INV-1: every bucketed figure ships beside the count of
    // rows that could NOT be bucketed, so a caller can never read a figure
    // computed over dated rows as a total. `shipped_*` counts only rows whose
    // status is actually shipped — an undated PLANNED row is not a missing
    // closure and must not inflate the gap.
    q.prepare(QStringLiteral(
        "SELECT "
        " SUM(CASE WHEN status='shipped' AND shipped IS NOT NULL THEN 1 ELSE 0 END), "
        " SUM(CASE WHEN status='shipped' AND shipped IS     NULL THEN 1 ELSE 0 END), "
        " SUM(CASE WHEN created IS NOT NULL THEN 1 ELSE 0 END), "
        " SUM(CASE WHEN created IS     NULL THEN 1 ELSE 0 END) "
        "FROM item WHERE 1=1%1").arg(rsScopeClause(projectId)));
    rsBindScope(q, projectId);
    if (!q.exec() || !q.next()) { if (error) *error = lastErr(q); return std::nullopt; }
    out.shippedDated   = q.value(0).toInt();
    out.shippedUndated = q.value(1).toInt();
    out.createdDated   = q.value(2).toInt();
    out.createdUndated = q.value(3).toInt();
    return out;
}

std::optional<RoadmapStore::WindowCounts>
RoadmapStore::countInWindow(std::optional<qint64> projectId, const QDate &from,
                            const QDate &untilExclusive, QString *error) const {
    // Half-open [from, untilExclusive) — INV-8. The columns are YYYY-MM-DD TEXT
    // and that format sorts lexicographically as it does chronologically, so a
    // string comparison IS a date comparison here; no julianday() needed, and
    // the CHECK constraint is what guarantees the format.
    const QString lo = from.toString(QStringLiteral("yyyy-MM-dd"));
    const QString hi = untilExclusive.toString(QStringLiteral("yyyy-MM-dd"));

    WindowCounts out;
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT "
        " SUM(CASE WHEN shipped IS NOT NULL AND shipped >= ? AND shipped < ? "
        "          THEN 1 ELSE 0 END), "
        " SUM(CASE WHEN created IS NOT NULL AND created >= ? AND created < ? "
        "          THEN 1 ELSE 0 END) "
        "FROM item WHERE 1=1%1").arg(rsScopeClause(projectId)));
    q.addBindValue(lo); q.addBindValue(hi);
    q.addBindValue(lo); q.addBindValue(hi);
    rsBindScope(q, projectId);
    if (!q.exec() || !q.next()) { if (error) *error = lastErr(q); return std::nullopt; }
    out.closed = q.value(0).toInt();
    out.added  = q.value(1).toInt();
    return out;
}

namespace {

// Median without loading the population. Two queries: count, then the middle
// row by LIMIT/OFFSET. O(1) memory rather than O(n) — ANTS-4501 § 4 makes that
// the rule for the backfill and the same discipline is free here.
//
// `dayExpr` is a SQL expression yielding a day count; `whereExtra` narrows the
// population. Both are internal literals, never caller text.
std::optional<RoadmapStore::DaySpread>
rsDaySpread(const QSqlDatabase &db, const std::optional<qint64> &projectId,
            const QString &dayExpr, const QVariantList &dayExprBinds,
            const QString &whereExtra, bool wantOldestAndOver90, QString *error) {
    auto lastErrLocal = [](const QSqlQuery &q) {
        return q.lastError().text().trimmed();
    };
    RoadmapStore::DaySpread out;
    QSqlQuery q(const_cast<QSqlDatabase &>(db));
    const QString scope = projectId ? QStringLiteral(" AND project_id = ?") : QString();
    const QString base  = QStringLiteral("FROM item WHERE 1=1%1%2")
                              .arg(whereExtra, scope);

    // `dayExprBinds` belong to dayExpr, which appears in the SELECT list — so
    // they are bound ONLY by the queries that carry it, and `nDayExpr` says how
    // many times. Binding them on the COUNT query, which has no placeholder,
    // is a parameter-count mismatch that makes every call return nullopt: the
    // defect this helper shipped with, and it failed uniformly rather than on
    // an edge case, which is what made it cheap to find.
    auto bindFor = [&](int nDayExpr) {
        for (int i = 0; i < nDayExpr; ++i)
            for (const QVariant &v : dayExprBinds) q.addBindValue(v);
        if (projectId) q.addBindValue(*projectId);
    };

    q.prepare(QStringLiteral("SELECT COUNT(*) %1").arg(base));
    bindFor(0);
    if (!q.exec() || !q.next()) { if (error) *error = lastErrLocal(q); return std::nullopt; }
    out.sample = q.value(0).toInt();
    if (out.sample == 0)
        return out;              // -1 medians stand: no sample, not zero days

    // Lower median on an even sample. Stated rather than left to the reader:
    // interpolating between two integers would report a half-day nobody can
    // point at a row for.
    const int offset = (out.sample - 1) / 2;
    q.prepare(QStringLiteral("SELECT %1 AS d %2 ORDER BY d LIMIT 1 OFFSET %3")
                  .arg(dayExpr, base, QString::number(offset)));
    bindFor(1);
    if (!q.exec() || !q.next()) { if (error) *error = lastErrLocal(q); return std::nullopt; }
    out.medianDays = q.value(0).toInt();

    if (wantOldestAndOver90) {
        // dayExpr twice — MAX() and the >90 CASE — so its binds go in twice.
        q.prepare(QStringLiteral(
            "SELECT MAX(%1), SUM(CASE WHEN %2 > 90 THEN 1 ELSE 0 END) %3")
                      .arg(dayExpr, dayExpr, base));
        bindFor(2);
        if (!q.exec() || !q.next()) { if (error) *error = lastErrLocal(q); return std::nullopt; }
        out.oldestDays = q.value(0).toInt();
        out.over90     = q.value(1).toInt();
    }
    return out;
}

}  // namespace

std::optional<RoadmapStore::DaySpread>
RoadmapStore::ageOfOpen(std::optional<qint64> projectId, const QDate &today,
                        QString *error) const {
    // julianday() over the two YYYY-MM-DD strings, floored to whole days. The
    // date column is date-only, so there is no time-of-day component to round.
    const QString dayExpr =
        QStringLiteral("CAST(julianday(?) - julianday(created) AS INTEGER)");
    return rsDaySpread(m_db, projectId, dayExpr,
                       {today.toString(QStringLiteral("yyyy-MM-dd"))},
                       QStringLiteral(" AND created IS NOT NULL%1")
                           .arg(QLatin1String(kOpenStatusIn)),
                       /*wantOldestAndOver90=*/true, error);
}

std::optional<RoadmapStore::DaySpread>
RoadmapStore::timeToClose(std::optional<qint64> projectId, QString *error) const {
    // BOTH dates known. That population is smaller than the shipped count, and
    // `sample` reporting it rather than the population is INV-9.
    return rsDaySpread(m_db, projectId,
                       QStringLiteral("CAST(julianday(shipped) - julianday(created) "
                                      "AS INTEGER)"),
                       {},
                       QStringLiteral(" AND shipped IS NOT NULL AND created IS NOT NULL"),
                       /*wantOldestAndOver90=*/false, error);
}

// ANTS-4501 § 2.3 — see the header for why `status` and the two NULL flags are
// the columns, and why this is a reader rather than SQL in the verb.
std::optional<QVector<RoadmapStore::DateTarget>>
RoadmapStore::itemsForDateBackfill(qint64 projectId, QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT item_pk, id, status, created IS NULL, shipped IS NULL "
        "FROM item WHERE project_id = ? ORDER BY item_pk"));
    q.addBindValue(projectId);
    if (!q.exec()) {
        if (error) *error = lastErr(q);
        return std::nullopt;
    }
    QVector<DateTarget> out;
    while (q.next()) {
        DateTarget t;
        t.itemPk        = q.value(0).toLongLong();
        t.id            = q.value(1).toString();
        t.status        = q.value(2).toString();
        t.createdIsNull = q.value(3).toBool();
        t.shippedIsNull = q.value(4).toBool();
        out.push_back(t);
    }
    return out;
}

std::optional<QVector<RoadmapStore::ItemRef>>
RoadmapStore::listItems(qint64 projectId, QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT i.item_pk, i.id_fold, i.headline, e.section_id, i.provenance "
        "FROM item i LEFT JOIN element e "
        "  ON e.item_pk = i.item_pk AND e.kind = 'item' "
        "WHERE i.project_id = ? ORDER BY i.item_pk"));
    q.addBindValue(projectId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }

    QVector<ItemRef> out;
    while (q.next()) {
        ItemRef r;
        r.itemPk = q.value(0).toLongLong();
        r.idFold = q.value(1).toString();
        r.headline = q.value(2).toString();
        r.sectionId = q.value(3).toLongLong();
        // Parsed here rather than with json_extract(): the JSON1 extension is a
        // compile-time option and this schema deliberately depends on no build
        // flags (§ 2.3's dbstat note is the same rule).
        r.idFromMigration =
            QJsonDocument::fromJson(q.value(4).toString().toUtf8())
                .object()
                .value(QStringLiteral("id")).toString() == QLatin1String("migrated");
        out.push_back(r);
    }
    return out;
}

std::optional<qint64> RoadmapStore::findSection(qint64 projectId, const QString &slug,
                                                QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT section_id FROM section WHERE project_id = ? AND slug = ?"));
    q.addBindValue(projectId);
    q.addBindValue(slug);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    return q.value(0).toLongLong();
}

// The column list both section readers select, and the hydration both apply.
// One shape rather than two byte-identical copies: ANTS-3817 added section_id
// to SectionRow, and with the copies in place a field populated in readSection()
// and forgotten in listSections() reads back as 0 — a valid-looking id that
// points at nothing. Mirrors itemFromRow() above.
static const char kSectionColumns[] =
    "SELECT slug, title, level, intro, parent_id, source_path, position, section_id "
    "FROM section ";

static RoadmapStore::SectionRow sectionFromRow(const QSqlQuery &q) {
    RoadmapStore::SectionRow s;
    s.slug = q.value(0).toString();
    s.title = q.value(1).toString();
    s.level = q.value(2).toInt();
    s.intro = q.value(3).toString();
    if (!q.value(4).isNull())
        s.parentId = q.value(4).toLongLong();
    // ANTS-3782 INV-26 — SQL NULL reads back as nullopt, anything else as the
    // stored string. Tested against a direct SELECT rather than through the
    // writer: a writer compared with its own idea of the value cannot
    // distinguish a stored value from a default.
    if (!q.value(5).isNull())
        s.sourcePath = q.value(5).toString();
    s.position = q.value(6).toInt();
    s.sectionId = q.value(7).toLongLong();
    return s;
}

std::optional<RoadmapStore::SectionRow> RoadmapStore::readSection(qint64 sectionId,
                                                                  QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QLatin1String(kSectionColumns) + QStringLiteral("WHERE section_id = ?"));
    q.addBindValue(sectionId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    return sectionFromRow(q);
}

std::optional<QVector<RoadmapStore::SectionRow>>
RoadmapStore::listSections(qint64 projectId, QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    // ORDER BY section_id, not by position: the ordering this enumeration feeds
    // is sectionOrderLess()'s, applied in C++ for the collation reason its
    // comment gives. A rowid order here is only for determinism between two
    // reads of the same store. listSectionsOrdered() below is the sorted one.
    q.prepare(QLatin1String(kSectionColumns)
              + QStringLiteral("WHERE project_id = ? ORDER BY section_id"));
    q.addBindValue(projectId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }

    QVector<SectionRow> out;
    while (q.next())
        out.push_back(sectionFromRow(q));
    return out;
}

// ANTS-3818. A wrapper and not a second query: the sort key is (position, slug)
// and slug ordering must be QString::compare()'s UTF-16 code-unit order, which
// an ORDER BY cannot express — SQLite's BINARY collation is UTF-8 byte order and
// the two disagree on the supplementary-plane characters an emoji heading slug
// reaches.
std::optional<QVector<RoadmapStore::SectionRow>>
RoadmapStore::listSectionsOrdered(qint64 projectId, QString *error) const {
    auto rows = listSections(projectId, error);
    if (!rows)
        return std::nullopt;
    std::sort(rows->begin(), rows->end(), sectionOrderLess);
    return rows;
}

// ANTS-3758 § 2.1. ORDER BY position and not by rowid: unlike listSections(),
// whose order is re-applied in C++ by sectionOrderLess(), element order IS
// position order — UNIQUE (section_id, position) already makes it total, so the
// SQL sort is the contract rather than a determinism aid.
std::optional<QVector<RoadmapStore::ElementRow>>
RoadmapStore::listElements(qint64 sectionId, QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.setForwardOnly(true);
    q.prepare(QStringLiteral(
        "SELECT e.position, e.kind, e.payload, e.item_pk, i.id_fold FROM element e "
        "LEFT JOIN item i ON i.item_pk = e.item_pk "
        "WHERE e.section_id = ? ORDER BY e.position"));
    q.addBindValue(sectionId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }

    QVector<ElementRow> out;
    while (q.next()) {
        ElementRow e;
        e.position = q.value(0).toInt();
        e.kind = q.value(1).toString();
        // NULL stays nullopt. The DDL makes payload NULL exactly for kind
        // 'item', and collapsing it to "" here would lose the distinction the
        // export's own emit path depends on.
        if (!q.value(2).isNull())
            e.payload = q.value(2).toString();
        if (!q.value(3).isNull())
            e.itemPk = q.value(3).toLongLong();
        e.itemIdFold = q.value(4).toString();
        out.push_back(e);
    }
    return out;
}

namespace {
// The two readProject* overloads differ only in their WHERE clause, so the row
// assembly lives once. A second copy would be the drift these readers exist to
// remove, one level down.
std::optional<RoadmapStore::ProjectRow> readProjectWhere(const QSqlDatabase &db,
                                                         const QString &column,
                                                         const QVariant &key,
                                                         QString *error) {
    QSqlQuery q(const_cast<QSqlDatabase &>(db));
    q.prepare(QStringLiteral("SELECT project_id, name, export_slug, legend, root, "
                             "source_format FROM project WHERE %1 = ?")
                  .arg(column));
    q.addBindValue(key);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;   // absent row is not an error, per idPrefixFor()

    RoadmapStore::ProjectRow p;
    p.projectId = q.value(0).toLongLong();
    p.name = q.value(1).toString();
    p.exportSlug = q.value(2).toString();
    p.legendText = q.value(3).toString();
    p.root = q.value(4).toString();
    p.sourceFormat = q.value(5).toString();
    return p;
}
} // namespace

std::optional<RoadmapStore::ProjectRow>
RoadmapStore::readProject(qint64 projectId, QString *error) const {
    return readProjectWhere(m_db, QStringLiteral("project_id"), projectId, error);
}

std::optional<RoadmapStore::ProjectRow>
RoadmapStore::readProjectBySlug(const QString &exportSlug, QString *error) const {
    return readProjectWhere(m_db, QStringLiteral("export_slug"), exportSlug, error);
}

std::optional<RoadmapStore::ProjectRow>
RoadmapStore::readProjectByRoot(const QString &canonicalRoot, QString *error) const {
    return readProjectWhere(m_db, QStringLiteral("root"), canonicalRoot, error);
}

bool sectionOrderLess(const RoadmapStore::SectionRow &a,
                      const RoadmapStore::SectionRow &b) {
    if (a.position != b.position)
        return a.position < b.position;
    return a.slug.compare(b.slug) < 0;
}

std::optional<QString> RoadmapStore::idPrefixFor(qint64 projectId,
                                                 QString *error) const {
    // Deterministic when a project somehow carries several: the busiest counter
    // wins, ties by prefix. A project has one prefix in practice, and an
    // arbitrary pick here would make the id a re-run allocates depend on row
    // order.
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT prefix FROM id_prefix WHERE project_id = ? "
        "ORDER BY high_water DESC, prefix ASC LIMIT 1"));
    q.addBindValue(projectId);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    return q.value(0).toString();
}

std::optional<qint64> RoadmapStore::idHighWater(qint64 projectId, const QString &prefix,
                                                QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT high_water FROM id_prefix WHERE project_id = ? AND prefix = ?"));
    q.addBindValue(projectId);
    q.addBindValue(prefix);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    return q.value(0).toLongLong();
}

std::optional<int> RoadmapStore::maxHistorySeq(qint64 itemPk, const QString &changedAt,
                                               QString *error) const {
    QSqlQuery q(const_cast<QSqlDatabase &>(m_db));
    q.prepare(QStringLiteral(
        "SELECT MAX(seq) FROM history WHERE item_pk = ? AND changed_at = ?"));
    q.addBindValue(itemPk);
    q.addBindValue(changedAt);
    if (!q.exec()) {
        if (error)
            *error = lastErr(q);
        return std::nullopt;
    }
    // MAX() over no rows is one row holding NULL, not zero rows — so the
    // absence is q.value(0).isNull(), and reading it as 0 would make the first
    // row of a stamp seq 1 and leave 0 permanently unused.
    if (!q.next() || q.value(0).isNull())
        return std::nullopt;
    return q.value(0).toInt();
}
