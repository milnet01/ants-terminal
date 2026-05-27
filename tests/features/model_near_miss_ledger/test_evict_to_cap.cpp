// ANTS-1894 INV-8 — appendRecord post-cap-overflow drops whole oldest
// lines; newest line never dropped; no mid-line truncation.
#include <gtest/gtest.h>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include "modelnearmissledger.h"

namespace NM = ModelNearMissLedger;

namespace {

NM::Record makeRecord(int seq) {
    NM::Record r;
    r.ts              = QStringLiteral("2026-05-27T09:30:%1Z").arg(seq, 2, 10, QLatin1Char('0'));
    r.sessionId       = QStringLiteral("session-%1").arg(seq);
    r.project         = QStringLiteral("/proj");
    r.currentTier     = QStringLiteral("opus");
    r.recommendedTier = QStringLiteral("haiku");
    r.blockedBy << QStringLiteral("composer_not_empty")
                << QStringLiteral("dwell_time_insufficient")
                << QStringLiteral("override_cooldown_active");
    r.composerEmpty   = false;
    r.focusedState    = QStringLiteral("thinking");
    r.ticksTargetStable = 1;
    r.dwellMs           = 45000;
    return r;
}

}  // namespace

TEST(ModelNearMissLedger, Inv8EvictionPreservesNewestAndCap) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));

    // Use a small cap so we don't need 1000 records.
    constexpr qint64 kSmallCap = 4 * 1024;   // 4 KiB

    // Write enough records to trigger eviction. Each serialised record is
    // ~250-350 B; 30 records ≈ 9 KiB.
    for (int i = 1; i <= 30; ++i) {
        ASSERT_TRUE(NM::appendRecord(path, makeRecord(i), kSmallCap));
    }

    // File ≤ cap.
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray content = f.readAll();
    EXPECT_LE(content.size(), kSmallCap);

    // Newest line (seq 30) is present.
    EXPECT_TRUE(content.contains("session-30"));
    // Oldest line (seq 1) has been evicted.
    EXPECT_FALSE(content.contains("\"session_id\":\"session-1\""));

    // Every surviving line parses as JSON (no mid-line truncation).
    const QList<NM::Record> recs = NM::readRecords(path);
    EXPECT_GT(recs.size(), 0);
    for (const NM::Record &r : recs) {
        EXPECT_FALSE(r.sessionId.isEmpty());
        EXPECT_EQ(r.project, QStringLiteral("/proj"));
    }
}

// Edge case: a single record larger than the cap is kept (newest never dropped).
TEST(ModelNearMissLedger, Inv8NewestKeptEvenWhenSoloOverCap) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    NM::Record r = makeRecord(1);
    // Inflate sessionId to force the single record over a tiny cap.
    r.sessionId = QString(2000, QChar('x'));

    ASSERT_TRUE(NM::appendRecord(path, r, /*capBytes=*/512));
    const QList<NM::Record> recs = NM::readRecords(path);
    ASSERT_EQ(recs.size(), 1);
    EXPECT_EQ(recs[0].sessionId.size(), 2000);
}
