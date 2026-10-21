// Feature-conformance test for ANTS-5516 — roadmap_query mode:"ledger".
// Contract: tests/features/roadmap_ledger/spec.md
//
// The pure half only: which in-progress items are stale, which commit
// subjects name no id, and which tag is the newest release. The verb that
// gathers store rows and git output is exercised end to end, not here.
#include <gtest/gtest.h>

#include "roadmapledger.h"

#include <QDate>

using RoadmapLedger::InProgressItem;

TEST(RoadmapLedger, StaleIsAtLeastStaleDaysUntouchedOldestFirstUndatedFirst) {
    const QDate today(2026, 10, 21);
    const QVector<InProgressItem> items = {
        {QStringLiteral("ANTS-1"), QStringLiteral("fresh"),    QStringLiteral("2026-10-08")},  // 13 days
        {QStringLiteral("ANTS-2"), QStringLiteral("boundary"), QStringLiteral("2026-10-07")},  // 14 days
        {QStringLiteral("ANTS-3"), QStringLiteral("old"),      QStringLiteral("2026-09-01")},
        {QStringLiteral("ANTS-4"), QStringLiteral("undated"),  QString()},
    };
    const auto stale = RoadmapLedger::staleInProgress(items, today, 14);
    ASSERT_EQ(3, stale.size());
    EXPECT_EQ(QStringLiteral("ANTS-4"), stale.at(0).id);
    EXPECT_EQ(-1, stale.at(0).days);
    EXPECT_EQ(QStringLiteral("ANTS-3"), stale.at(1).id);
    EXPECT_EQ(50, stale.at(1).days);
    EXPECT_EQ(QStringLiteral("ANTS-2"), stale.at(2).id);
    EXPECT_EQ(14, stale.at(2).days);
}

TEST(RoadmapLedger, CommitsWithoutIdMatchTheWholeIdOnly) {
    const QByteArray log =
        "a1\tANTS-5507: changelog_query lint\n"
        "b2\tRoadmap: close ANTS-4945 (resolved by ANTS-5098)\n"
        "c3\tCLAUDE.md: other sessions' requests move up\n"
        "d4\tXANTS-12 is another project's id\n"
        "e5\tFix ANTS-12a typo\n";
    const auto rows = RoadmapLedger::commitsWithoutId(log, QStringLiteral("ANTS"));
    ASSERT_EQ(3, rows.size());
    EXPECT_EQ(QStringLiteral("c3"), rows.at(0).sha);
    EXPECT_EQ(QStringLiteral("CLAUDE.md: other sessions' requests move up"), rows.at(0).subject);
    EXPECT_EQ(QStringLiteral("d4"), rows.at(1).sha);
    EXPECT_EQ(QStringLiteral("e5"), rows.at(2).sha);
}

TEST(RoadmapLedger, NewestStableTagComparesNumbersAndSkipsSuffixes) {
    const QByteArray refs = "v0.7.99\nv0.7.114\nv0.8.0-rc1\nnightly\nv0.7.2\n";
    EXPECT_EQ(QStringLiteral("v0.7.114"), RoadmapLedger::newestStableTag(refs));
    EXPECT_TRUE(RoadmapLedger::newestStableTag("nightly\n").isEmpty());
}
