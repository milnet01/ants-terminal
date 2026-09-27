// Feature-conformance test for ANTS-4499 — the pre-migration store snapshot.
// Contract: tests/features/roadmap_migrate_backup/spec.md
//
// The probe table is created by the test rather than borrowed from the roadmap
// schema. The claim under test is about the SNAPSHOT MECHANISM — that it
// carries what the live connection wrote, including rows still in the -wal —
// and a fixture built on the real schema would couple that claim to a shape
// that changes for unrelated reasons.

#include <gtest/gtest.h>

#include "roadmapmigrateverb.h"
#include "roadmapstore.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

namespace {

// Reads one scalar back out of a snapshot on its own connection. Scoped so the
// QSqlQuery is destroyed before removeDatabase(), which Qt warns about
// otherwise and which would leave the connection alive for the next test.
QString readProbe(const QString &path, const QString &connName) {
    QString out = QStringLiteral("<unopened>");
    {
        QSqlDatabase copy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        copy.setDatabaseName(path);
        if (!copy.open())
            return QStringLiteral("<open failed>");
        QSqlQuery r(copy);
        if (!r.exec(QStringLiteral("SELECT v FROM snapshot_probe")) || !r.next())
            out = QStringLiteral("<no row>");
        else
            out = r.value(0).toString();
        copy.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return out;
}

void writeProbe(RoadmapStore &store, const QString &value, bool create) {
    QSqlQuery q(store.db());
    // Braced: ASSERT_TRUE expands to an if/else, so an unbraced body here is a
    // dangling else.
    if (create) {
        ASSERT_TRUE(q.exec(QStringLiteral("CREATE TABLE snapshot_probe (v TEXT)")));
    }
    ASSERT_TRUE(q.exec(QStringLiteral("DELETE FROM snapshot_probe")));
    q.prepare(QStringLiteral("INSERT INTO snapshot_probe (v) VALUES (?)"));
    q.addBindValue(value);
    ASSERT_TRUE(q.exec());
}

}  // namespace

// INV-1 — the snapshot carries rows that are committed but still resident in
// the -wal. No checkpoint is forced, which is the whole point: that is the
// state in which a plain file copy silently loses the most recent writes, and
// it is the trap the reporting session documented.
TEST(RoadmapMigrateBackup, Inv1SnapshotCarriesWalResidentRows) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RoadmapStore store(dir.path() + QStringLiteral("/roadmap.sqlite"));
    ASSERT_TRUE(store.open());
    writeProbe(store, QStringLiteral("written-before-snapshot"), true);

    const QString dest = dir.path() + QStringLiteral("/pre-migrate.sqlite");
    QString err;
    ASSERT_TRUE(store.snapshotTo(dest, &err)) << err.toStdString();
    ASSERT_TRUE(QFile::exists(dest));

    EXPECT_EQ(readProbe(dest, QStringLiteral("probe_inv1")).toStdString(),
              std::string("written-before-snapshot"));
}

// INV-2 — mode 0600. One snapshot holds every project's roadmap on the machine.
TEST(RoadmapMigrateBackup, Inv2SnapshotIsOwnerOnly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RoadmapStore store(dir.path() + QStringLiteral("/roadmap.sqlite"));
    ASSERT_TRUE(store.open());
    writeProbe(store, QStringLiteral("x"), true);

    const QString dest = dir.path() + QStringLiteral("/pre-migrate.sqlite");
    ASSERT_TRUE(store.snapshotTo(dest));

    const QFileDevice::Permissions p = QFile::permissions(dest);
    EXPECT_TRUE(p.testFlag(QFileDevice::ReadOwner));
    EXPECT_TRUE(p.testFlag(QFileDevice::WriteOwner));
    // The half that matters: nobody else can read the machine's whole roadmap.
    EXPECT_FALSE(p.testFlag(QFileDevice::ReadGroup));
    EXPECT_FALSE(p.testFlag(QFileDevice::ReadOther));
}

// INV-3 / INV-4 — rolling, and no debris. The second snapshot replaces the
// first, and the temp file the write went through is not left behind. A
// caller listing the directory sees one snapshot, never two or a `.partial`.
TEST(RoadmapMigrateBackup, Inv3And4SnapshotRollsAndLeavesNoPartial) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RoadmapStore store(dir.path() + QStringLiteral("/roadmap.sqlite"));
    ASSERT_TRUE(store.open());

    const QString dest = dir.path() + QStringLiteral("/pre-migrate.sqlite");

    writeProbe(store, QStringLiteral("first"), true);
    ASSERT_TRUE(store.snapshotTo(dest));
    EXPECT_EQ(readProbe(dest, QStringLiteral("probe_first")).toStdString(),
              std::string("first"));

    writeProbe(store, QStringLiteral("second"), false);
    ASSERT_TRUE(store.snapshotTo(dest));
    EXPECT_EQ(readProbe(dest, QStringLiteral("probe_second")).toStdString(),
              std::string("second"));

    EXPECT_FALSE(QFile::exists(dest + QStringLiteral(".partial")));
}

// INV-9 (ANTS-5466) — a file at the destination that is not a SQLite database
// is refused and left as it was; so is one at the temp path. An empty temp
// file is what a killed run leaves, and is cleared.
TEST(RoadmapMigrateBackup, Inv9RefusesToReplaceAFileThatIsNotADatabase) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RoadmapStore store(dir.path() + QStringLiteral("/roadmap.sqlite"));
    ASSERT_TRUE(store.open());
    writeProbe(store, QStringLiteral("x"), true);

    const QByteArray precious("notes the user wanted to keep\n");
    const auto plant = [&](const QString &path, const QByteArray &bytes) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
    };
    const auto contents = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
    };

    // At the destination.
    const QString dest = dir.path() + QStringLiteral("/notes.txt");
    ASSERT_TRUE(plant(dest, precious));
    QString err;
    EXPECT_FALSE(store.snapshotTo(dest, &err));
    EXPECT_TRUE(err.contains(dest)) << err.toStdString();
    EXPECT_EQ(contents(dest), precious);

    // At the temp path beside a free destination.
    const QString dest2 = dir.path() + QStringLiteral("/snap.sqlite");
    const QString partial = dest2 + QStringLiteral(".partial");
    ASSERT_TRUE(plant(partial, precious));
    err.clear();
    EXPECT_FALSE(store.snapshotTo(dest2, &err));
    EXPECT_TRUE(err.contains(partial)) << err.toStdString();
    EXPECT_EQ(contents(partial), precious);
    EXPECT_FALSE(QFile::exists(dest2));

    // An empty temp file is cleared, and the snapshot is taken.
    ASSERT_TRUE(plant(partial, QByteArray()));
    err.clear();
    EXPECT_TRUE(store.snapshotTo(dest2, &err)) << err.toStdString();
    EXPECT_EQ(readProbe(dest2, QStringLiteral("probe_inv9")).toStdString(), std::string("x"));
}

// INV-5 — the rolling snapshot must not answer to `roadmap-*.sqlite`. That
// glob is what tools/roadmap-store-backup.sh prunes to its KEEP limit, so a
// matching name would quietly cost the weekly rotation one kept snapshot.
// Asserted on the name the verb defaults to, since that is the one nobody
// chooses deliberately.
TEST(RoadmapMigrateBackup, Inv5DefaultNameIsOutsideTheWeeklyPruneGlob) {
    const QString base = QFileInfo(RoadmapStore::defaultSnapshotPath()).fileName();
    EXPECT_FALSE(base.startsWith(QStringLiteral("roadmap-")))
        << "default snapshot name '" << base.toStdString()
        << "' matches the weekly prune glob roadmap-*.sqlite";
    EXPECT_TRUE(base.endsWith(QStringLiteral(".sqlite")));
}

// ---------------------------------------------------------- the verb half ---

namespace {

// Deliberately a local copy of roadmap_migrate_verb's fixture rather than a
// shared header: these two suites pin different contracts, and a shared fixture
// makes a change made for one of them quietly alter the other's subject.
bool writeFileAt(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(bytes) == bytes.size();
}

QString makeProjectRoot(const QTemporaryDir &dir, const QString &leaf) {
    const QString root = dir.filePath(leaf);
    const QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **An open item.**\n"
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: test.\n";
    if (!writeFileAt(root + QStringLiteral("/ROADMAP.md"), md))
        return QString();
    return QFileInfo(root).canonicalFilePath();
}

RoadmapMigrateVerb::Request migrateRequest(const QString &root) {
    RoadmapMigrateVerb::Request r;
    r.projectRoot = root;
    r.projectName = QStringLiteral("Demo");
    r.exportSlug  = QStringLiteral("demo");
    r.changedAt   = QStringLiteral("2026-09-20T10:00:00Z");
    return r;
}

int projectCount(const QString &storePath, const QString &connName) {
    int n = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(storePath);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT COUNT(*) FROM project")) && q.next())
                n = q.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return n;
}

}  // namespace

// INV-6 — a real run snapshots BEFORE it opens its transaction, and says where.
//
// The pre-migration state is what proves the ordering: this is a first
// migration into an empty store, so the snapshot must hold no project while the
// store afterwards holds one. A snapshot taken after the load would hold the
// migrated row and protect nothing.
TEST(RoadmapMigrateBackup, Inv6RealRunSnapshotsBeforeTheTransaction) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));
    const QString dest = dir.filePath(QStringLiteral("backups/pre-migrate.sqlite"));

    auto req = migrateRequest(root);
    req.backupTo = dest;
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();

    EXPECT_TRUE(env.value(QStringLiteral("backup_taken")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("backup_path")).toString().toStdString(),
              dest.toStdString());
    ASSERT_TRUE(QFile::exists(dest));

    EXPECT_EQ(projectCount(dest, QStringLiteral("bk_before")), 0)
        << "the snapshot holds the migrated row, so it was taken after the load";
    EXPECT_EQ(projectCount(storePath, QStringLiteral("bk_after")), 1);
}

// INV-7 — a dry run takes none. It commits nothing, so there is nothing to
// protect, and spending the ROLLING snapshot on a preview would destroy the one
// taken before the last real migration.
TEST(RoadmapMigrateBackup, Inv7DryRunTakesNoSnapshot) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString dest = dir.filePath(QStringLiteral("backups/pre-migrate.sqlite"));

    auto req = migrateRequest(root);
    req.backupTo = dest;
    req.dryRun   = true;
    const QJsonObject env =
        RoadmapMigrateVerb::run(dir.filePath(QStringLiteral("store.sqlite")), req);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();

    EXPECT_FALSE(env.value(QStringLiteral("backup_taken")).toBool());
    EXPECT_FALSE(QFile::exists(dest));
}

// INV-8 — a failed snapshot refuses the migration outright.
//
// Proceeding past a failed backup is the one outcome nobody would choose
// knowingly, so it is not a warning. The destination here is unwritable because
// its parent is a regular file, which mkpath cannot turn into a directory.
TEST(RoadmapMigrateBackup, Inv8FailedSnapshotRefusesAndMigratesNothing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    const QString blocker = dir.filePath(QStringLiteral("blocker"));
    ASSERT_TRUE(writeFileAt(blocker, QByteArrayLiteral("not a directory")));

    auto req = migrateRequest(root);
    req.backupTo = blocker + QStringLiteral("/nested/pre-migrate.sqlite");
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, req);

    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("code")).toString().toStdString(),
              std::string("backup_failed"));
    // The refusal is only worth anything if it really stopped the migration.
    EXPECT_NE(projectCount(storePath, QStringLiteral("bk_refused")), 1);
}

// INV-8's escape hatch: `backup:false` is the caller choosing to migrate
// unprotected in so many words, and it must actually work -- otherwise the
// refusal above is a wall with no door.
TEST(RoadmapMigrateBackup, Inv8BackupFalseMigratesWithoutOne) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store.sqlite"));

    auto req = migrateRequest(root);
    req.backup = false;
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_FALSE(env.value(QStringLiteral("backup_taken")).toBool());
    EXPECT_EQ(projectCount(storePath, QStringLiteral("bk_none")), 1);
}

// ANTS-5247 — with no backup_to, the snapshot goes to Request::snapshotDir,
// and beside the store when that is empty or fails. ANTS-3855 § 2.4, INV-15.
namespace {

QString besideStore(const QString &storePath) {
    return QFileInfo(storePath).absolutePath() + QStringLiteral("/pre-migrate.sqlite");
}

}  // namespace

// INV-10 — a folder that exists is used, and the reply names its rung.
TEST(RoadmapMigrateBackup, Inv10SnapshotDirIsUsed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store/store.sqlite"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(storePath).absolutePath()));
    const QString snaps = dir.filePath(QStringLiteral("snaps"));
    ASSERT_TRUE(QDir().mkpath(snaps));

    auto req = migrateRequest(root);
    req.snapshotDir = snaps;
    req.snapshotDirSource = QStringLiteral("config");
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("backup_path")).toString().toStdString(),
              (snaps + QStringLiteral("/pre-migrate.sqlite")).toStdString());
    EXPECT_EQ(env.value(QStringLiteral("backup_path_source")).toString().toStdString(),
              std::string("config"));
    EXPECT_FALSE(env.contains(QStringLiteral("backup_fallback")));
    EXPECT_TRUE(QFile::exists(snaps + QStringLiteral("/pre-migrate.sqlite")));
}

// INV-11 — no folder named: beside storePath, never beside the real store.
TEST(RoadmapMigrateBackup, Inv11EmptySnapshotDirGoesBesideTheStore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store/store.sqlite"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(storePath).absolutePath()));

    const QJsonObject env = RoadmapMigrateVerb::run(storePath, migrateRequest(root));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("backup_path")).toString().toStdString(),
              besideStore(storePath).toStdString());
    EXPECT_EQ(env.value(QStringLiteral("backup_path_source")).toString().toStdString(),
              std::string("beside_store"));
    EXPECT_TRUE(QFile::exists(besideStore(storePath)));
}

// INV-12 — a snapshot to the folder that fails is retaken beside the store,
// and the reply says what was tried. The failure is ANTS-5466's refusal to
// replace a file that is not a database, which holds for any user, root too.
TEST(RoadmapMigrateBackup, Inv12FailedFolderFallsBackBesideTheStore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store/store.sqlite"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(storePath).absolutePath()));
    const QString snaps = dir.filePath(QStringLiteral("snaps"));
    ASSERT_TRUE(QDir().mkpath(snaps));
    ASSERT_TRUE(writeFileAt(snaps + QStringLiteral("/pre-migrate.sqlite"),
                            QByteArrayLiteral("not a database")));

    auto req = migrateRequest(root);
    req.snapshotDir = snaps;
    req.snapshotDirSource = QStringLiteral("backup_record");
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("backup_path")).toString().toStdString(),
              besideStore(storePath).toStdString());
    EXPECT_EQ(env.value(QStringLiteral("backup_path_source")).toString().toStdString(),
              std::string("beside_store"));
    const QJsonObject fb = env.value(QStringLiteral("backup_fallback")).toObject();
    EXPECT_EQ(fb.value(QStringLiteral("folder")).toString().toStdString(), snaps.toStdString());
    EXPECT_FALSE(fb.value(QStringLiteral("error")).toString().isEmpty());
}

// INV-13 — a folder that does not exist is never created: a folder on an
// unmounted drive would be recreated on the system drive. It falls back.
TEST(RoadmapMigrateBackup, Inv13MissingFolderIsNotCreated) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = makeProjectRoot(dir, QStringLiteral("proj"));
    ASSERT_FALSE(root.isEmpty());
    const QString storePath = dir.filePath(QStringLiteral("store/store.sqlite"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(storePath).absolutePath()));
    const QString gone = dir.filePath(QStringLiteral("unmounted/snaps"));

    auto req = migrateRequest(root);
    req.snapshotDir = gone;
    req.snapshotDirSource = QStringLiteral("config");
    const QJsonObject env = RoadmapMigrateVerb::run(storePath, req);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_FALSE(QFileInfo::exists(gone)) << "the missing folder was created";
    EXPECT_EQ(env.value(QStringLiteral("backup_path")).toString().toStdString(),
              besideStore(storePath).toStdString());
    EXPECT_EQ(env.value(QStringLiteral("backup_fallback")).toObject()
                  .value(QStringLiteral("folder")).toString().toStdString(),
              gone.toStdString());
}
