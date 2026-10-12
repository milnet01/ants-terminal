// Feature-conformance test for ANTS-3794 INV-11 — the backup health check.
// Contract: tests/features/roadmap_backup_health/spec.md

#include <gtest/gtest.h>

#include "roadmapbackuphealth.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>

namespace {

QDateTime now() { return QDateTime(QDate(2026, 9, 21), QTime(12, 0, 0), QTimeZone::utc()); }

QString iso(const QDateTime &t) { return t.toString(Qt::ISODate); }

void writeRecord(const QString &dir, const QString &job, const QString &attempt,
                 const QString &success, const QString &error) {
    QFile f(dir + QStringLiteral("/roadmap-backup-") + job + QStringLiteral(".state"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QStringLiteral("attempt=%1\nsuccess=%2\nerror=%3\n")
                .arg(attempt, success, error).toUtf8());
}

void writeHealthy(const QString &dir, const QString &job) {
    const QString t = iso(now().addDays(-2));
    writeRecord(dir, job, t, t, QString());
}

QString stateOf(const QJsonObject &o, const QString &job) {
    return o.value(QStringLiteral("jobs")).toObject().value(job).toObject()
        .value(QStringLiteral("state")).toString();
}

}  // namespace

TEST(RoadmapBackupHealth, BothHealthyIsEmpty) {
    QTemporaryDir d;
    writeHealthy(d.path(), QStringLiteral("snapshot"));
    writeHealthy(d.path(), QStringLiteral("export"));
    EXPECT_TRUE(RoadmapBackupHealth::assess(d.path(), now(), true).isEmpty());
}

TEST(RoadmapBackupHealth, NoStoreIsEmptyEvenWithoutRecords) {
    QTemporaryDir d;
    EXPECT_TRUE(RoadmapBackupHealth::assess(d.path(), now(), false).isEmpty());
}

TEST(RoadmapBackupHealth, MissingRecordIsNeverRun) {
    QTemporaryDir d;
    writeHealthy(d.path(), QStringLiteral("snapshot"));
    const QJsonObject o = RoadmapBackupHealth::assess(d.path(), now(), true);
    EXPECT_EQ(stateOf(o, QStringLiteral("export")), QStringLiteral("never_run"));
    EXPECT_FALSE(o.value(QStringLiteral("jobs")).toObject().contains(QStringLiteral("snapshot")))
        << "a healthy job must not be listed";
    EXPECT_TRUE(o.value(QStringLiteral("hint")).toString().contains(
        QStringLiteral("ants-roadmap-export.timer")));
    EXPECT_FALSE(o.value(QStringLiteral("hint")).toString().contains(
        QStringLiteral("ants-roadmap-backup.timer")));
}

TEST(RoadmapBackupHealth, ErrorIsFailingEvenWithARecentSuccess) {
    QTemporaryDir d;
    writeHealthy(d.path(), QStringLiteral("export"));
    const QString t = iso(now().addDays(-1));
    writeRecord(d.path(), QStringLiteral("snapshot"), t, t, QStringLiteral("disk full"));
    const QJsonObject o = RoadmapBackupHealth::assess(d.path(), now(), true);
    EXPECT_EQ(stateOf(o, QStringLiteral("snapshot")), QStringLiteral("failing"));
    EXPECT_EQ(o.value(QStringLiteral("jobs")).toObject().value(QStringLiteral("snapshot"))
                  .toObject().value(QStringLiteral("error")).toString(),
              QStringLiteral("disk full"));
    EXPECT_TRUE(o.value(QStringLiteral("hint")).toString().contains(
        QStringLiteral("ants-roadmap-backup.timer")));
}

TEST(RoadmapBackupHealth, FailedFirstRunIsFailingNotNeverRun) {
    QTemporaryDir d;
    writeHealthy(d.path(), QStringLiteral("snapshot"));
    writeRecord(d.path(), QStringLiteral("export"), iso(now().addDays(-1)), QString(),
                QStringLiteral("push rejected"));
    EXPECT_EQ(stateOf(RoadmapBackupHealth::assess(d.path(), now(), true),
                      QStringLiteral("export")),
              QStringLiteral("failing"));
}

TEST(RoadmapBackupHealth, StaleBoundaryBothSides) {
    QTemporaryDir d;
    writeHealthy(d.path(), QStringLiteral("snapshot"));
    const qint64 cap = RoadmapBackupHealth::kStaleAfterSecs;

    const QString inside = iso(now().addSecs(-cap + 60));
    writeRecord(d.path(), QStringLiteral("export"), inside, inside, QString());
    EXPECT_TRUE(RoadmapBackupHealth::assess(d.path(), now(), true).isEmpty());

    const QString outside = iso(now().addSecs(-cap - 60));
    writeRecord(d.path(), QStringLiteral("export"), outside, outside, QString());
    EXPECT_EQ(stateOf(RoadmapBackupHealth::assess(d.path(), now(), true),
                      QStringLiteral("export")),
              QStringLiteral("stale"));
}

TEST(RoadmapBackupHealth, EmptySuccessWithoutErrorIsStale) {
    QTemporaryDir d;
    writeHealthy(d.path(), QStringLiteral("snapshot"));
    writeRecord(d.path(), QStringLiteral("export"), iso(now().addDays(-1)), QString(), QString());
    EXPECT_EQ(stateOf(RoadmapBackupHealth::assess(d.path(), now(), true),
                      QStringLiteral("export")),
              QStringLiteral("stale"));
}

// ANTS-5247 — snapshotDest() owns ANTS-3855 § 2.4 rungs 2 and 3: the config
// key wins, then the snapshot record's dest=, and an empty value is unset.
TEST(RoadmapBackupHealth, Ants5247SnapshotDestOrder) {
    QTemporaryDir d;
    const QString rec = d.path() + QStringLiteral("/roadmap-backup-snapshot.state");
    const auto writeSnap = [&](const QString &dest) {
        QFile f(rec);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QStringLiteral("attempt=%1\nsuccess=%1\nerror=\ndest=%2\n")
                    .arg(iso(now()), dest).toUtf8());
    };

    // No record at all: neither rung names a folder.
    auto r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), QString());
    EXPECT_TRUE(r.folder.isEmpty());
    EXPECT_TRUE(r.source.isEmpty());

    writeSnap(QStringLiteral("/srv/backups/roadmap"));
    r = RoadmapBackupHealth::snapshotDest(QStringLiteral("/cfg/snaps"), d.path(), QString());
    EXPECT_EQ(r.folder, QStringLiteral("/cfg/snaps")) << "the config key must win";
    EXPECT_EQ(r.source, QStringLiteral("config"));

    r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), QString());
    EXPECT_EQ(r.folder, QStringLiteral("/srv/backups/roadmap"));
    EXPECT_EQ(r.source, QStringLiteral("backup_record"));

    // dest= present but empty (no success yet) reads as unset.
    writeSnap(QString());
    r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), QString());
    EXPECT_TRUE(r.folder.isEmpty());
    EXPECT_TRUE(r.source.isEmpty()) << "an empty dest= must not report backup_record";
}

// ANTS-5621 — rung 3 is the record's dest= only when its store= names the
// store being migrated, or is absent or empty (a record from before
// ANTS-5621). A migrate into a scratch store overwrote the real store's
// pre-migrate.sqlite because the record named no store. Both sides compare the
// store directory's canonical path plus the file name, so a store file not yet
// created still compares, and a symlinked directory still matches.
TEST(RoadmapBackupHealth, Ants5621SnapshotDestNamesItsStore) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    ASSERT_TRUE(QDir(d.path()).mkpath(QStringLiteral("real")));
    ASSERT_TRUE(QDir(d.path()).mkpath(QStringLiteral("scratch")));
    ASSERT_TRUE(QFile::link(d.path() + QStringLiteral("/real"),
                            d.path() + QStringLiteral("/alias")));
    const QString realDir = QDir(d.path() + QStringLiteral("/real")).canonicalPath();
    const QString realStore    = d.path() + QStringLiteral("/real/roadmap.sqlite");
    const QString aliasStore   = d.path() + QStringLiteral("/alias/roadmap.sqlite");
    const QString scratchStore = d.path() + QStringLiteral("/scratch/roadmap.sqlite");
    const QString rec = d.path() + QStringLiteral("/roadmap-backup-snapshot.state");
    const auto writeSnap = [&](const QString &storeLine) {
        QFile f(rec);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QStringLiteral("attempt=%1\nsuccess=%1\nerror=\ndest=/srv/snaps\n%2")
                    .arg(iso(now()), storeLine).toUtf8());
    };

    // The record names the real store, as the weekly job writes it.
    writeSnap(QStringLiteral("store=%1/roadmap.sqlite\n").arg(realDir));
    auto r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), realStore);
    EXPECT_EQ(r.source, QStringLiteral("backup_record")) << "the matching store uses rung 3";
    EXPECT_EQ(r.folder, QStringLiteral("/srv/snaps"));
    r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), aliasStore);
    EXPECT_EQ(r.source, QStringLiteral("backup_record")) << "a symlinked directory still matches";

    // The defect: a scratch store must not take the real store's folder.
    r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), scratchStore);
    EXPECT_TRUE(r.source.isEmpty()) << "a record naming another store must be skipped";
    EXPECT_TRUE(r.folder.isEmpty());

    // A record from before ANTS-5621: no store= line, or an empty one.
    writeSnap(QString());
    r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), scratchStore);
    EXPECT_EQ(r.source, QStringLiteral("backup_record")) << "no store= line is used as before";
    writeSnap(QStringLiteral("store=\n"));
    r = RoadmapBackupHealth::snapshotDest(QString(), d.path(), scratchStore);
    EXPECT_EQ(r.source, QStringLiteral("backup_record")) << "an empty store= reads as absent";
}
