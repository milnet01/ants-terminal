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

// ANTS-2119 — the incremental evictToCap drops a contiguous OLDEST prefix in a
// single erase (running byte total, not an O(N²) re-sum + front-erase loop).
// The survivors must therefore be an unbroken suffix ending at the newest
// record: no middle line dropped, no off-by-one at the erase boundary.
TEST(ModelNearMissLedger, Ants2119EvictionDropsContiguousOldestPrefix) {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nearmiss.jsonl"));
    constexpr qint64 kSmallCap = 4 * 1024;   // forces several drops at 40 records

    for (int i = 1; i <= 40; ++i)
        ASSERT_TRUE(NM::appendRecord(path, makeRecord(i), kSmallCap));

    const QList<NM::Record> recs = NM::readRecords(path);
    ASSERT_GT(recs.size(), 1);
    EXPECT_LT(recs.size(), 40) << "cap must have forced at least one eviction";

    // Survivors are a contiguous run ending at the newest (seq 40); parse the
    // trailing integer of each "session-<n>" id.
    const qsizetype prefix = QStringLiteral("session-").size();
    int prev = -1;
    for (const NM::Record &r : recs) {
        const int seq = r.sessionId.mid(prefix).toInt();
        if (prev >= 0)
            EXPECT_EQ(seq, prev + 1) << "survivors must be a contiguous prefix-drop";
        prev = seq;
    }
    EXPECT_EQ(prev, 40) << "newest record (seq 40) must be the last survivor";
    EXPECT_LE(QFile(path).size(), kSmallCap);
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
