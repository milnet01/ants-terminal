// Feature-conformance test for ANTS-3794 INV-11 — the backup health check.
// Contract: tests/features/roadmap_backup_health/spec.md

#include <gtest/gtest.h>

#include "roadmapbackuphealth.h"

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
