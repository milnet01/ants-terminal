// ANTS-3579 — pure-function tests for the per-project tokens-saved helpers
// (TokenUsageEngine::foldProjectBucket / pruneProjectBuckets) + the promoted
// public kCharsPerToken. Source-scrape wiring tests for recordDispatch / the
// widget / the fold live alongside; this file pins the pure math.
// See docs/specs/ANTS-3579.md §§ 2.5 / 5, INV-1/5/5b/9.

#include "tokenusageengine.h"

#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>

using namespace TokenUsageEngine;

namespace {

qint64 lifetimeOf(const QJsonObject &byProject, const QString &root) {
    return static_cast<qint64>(
        byProject.value(root).toObject().value("lifetime").toDouble(0));
}
QString sinceOf(const QJsonObject &byProject, const QString &root) {
    return byProject.value(root).toObject().value("since").toString();
}
QString updatedOf(const QJsonObject &byProject, const QString &root) {
    return byProject.value(root).toObject().value("updated").toString();
}
int monthlyCount(const QJsonObject &byProject, const QString &root) {
    return byProject.value(root).toObject().value("monthly").toObject().size();
}

}  // namespace

// INV-9 — empty start: one fold yields exactly one root at the session total.
TEST(TokensSavedByProject, FoldEmptyStartOneRoot) {
    QJsonObject bp;
    bp = foldProjectBucket(bp, "/p/x", 1000, "2026-07",
                           "2026-07-20T09:35:00", /*keepMonths=*/24);
    EXPECT_EQ(bp.size(), 1);
    EXPECT_EQ(lifetimeOf(bp, "/p/x"), 1000);
    EXPECT_EQ(sinceOf(bp, "/p/x"), QString("2026-07-20"));  // date portion (L-3)
    EXPECT_EQ(updatedOf(bp, "/p/x"), QString("2026-07-20T09:35:00"));
    EXPECT_EQ(monthlyCount(bp, "/p/x"), 1);
}

// Accumulate: lifetime sums; `since` sticks to the first fold; `updated` advances.
TEST(TokensSavedByProject, FoldAccumulatesSinceSticksUpdatedAdvances) {
    QJsonObject bp;
    bp = foldProjectBucket(bp, "/p/x", 1000, "2026-07", "2026-07-20T09:00:00", 24);
    bp = foldProjectBucket(bp, "/p/x",  500, "2026-08", "2026-08-01T10:00:00", 24);
    EXPECT_EQ(lifetimeOf(bp, "/p/x"), 1500);
    EXPECT_EQ(sinceOf(bp, "/p/x"), QString("2026-07-20"));           // unchanged
    EXPECT_EQ(updatedOf(bp, "/p/x"), QString("2026-08-01T10:00:00")); // advanced
    EXPECT_EQ(monthlyCount(bp, "/p/x"), 2);
}

// INV-5 (month prune) — foldProjectBucket threads foldMonthlyBucket's 24-cap.
TEST(TokensSavedByProject, FoldPrunesMonthsAt24) {
    QJsonObject bp;
    for (int m = 1; m <= 30; ++m) {  // 2024-01 .. 2026-06 (30 distinct months)
        const QString key = QString("%1-%2")
            .arg(2024 + (m - 1) / 12)
            .arg((m - 1) % 12 + 1, 2, 10, QLatin1Char('0'));
        bp = foldProjectBucket(bp, "/p/x", 100, key,
                               key + "-01T00:00:00", /*keepMonths=*/24);
    }
    EXPECT_EQ(monthlyCount(bp, "/p/x"), 24);      // capped
    EXPECT_EQ(lifetimeOf(bp, "/p/x"), 30 * 100);  // lifetime is never pruned
}

// M-2 — foldProjectBucket ALONE never evicts roots (65 roots all survive).
TEST(TokensSavedByProject, FoldNeverEvictsRoots) {
    QJsonObject bp;
    for (int i = 0; i < 65; ++i)
        bp = foldProjectBucket(bp, QString("/p/%1").arg(i), 10, "2026-07",
                               "2026-07-20T09:35:00", 24);
    EXPECT_EQ(bp.size(), 65);  // no eviction here — that's pruneProjectBuckets
}

// INV-5 — pruneProjectBuckets keeps the newest `updated` and drops the oldest.
TEST(TokensSavedByProject, PruneKeepsNewestByUpdated) {
    QJsonObject bp;
    for (int i = 0; i < 65; ++i)  // updated = ...T00, T01, ... distinct + ascending
        bp = foldProjectBucket(bp, QString("/p/%1").arg(i), 10, "2026-07",
                               QString("2026-07-20T%1:00:00")
                                   .arg(i, 2, 10, QLatin1Char('0')),
                               24);
    bp = pruneProjectBuckets(bp, 64);
    EXPECT_EQ(bp.size(), 64);
    EXPECT_FALSE(bp.contains("/p/0"));  // oldest updated (T00) dropped
    EXPECT_TRUE(bp.contains("/p/64"));  // newest kept
}

// INV-5b — same `updated` → deterministic tie-break (larger root string evicted).
TEST(TokensSavedByProject, PruneTieBreakDeterministic) {
    QJsonObject bp;
    const QString iso = "2026-07-20T09:35:00";  // identical for all
    bp = foldProjectBucket(bp, "/p/a", 10, "2026-07", iso, 24);
    bp = foldProjectBucket(bp, "/p/b", 10, "2026-07", iso, 24);
    bp = foldProjectBucket(bp, "/p/c", 10, "2026-07", iso, 24);
    const QJsonObject pruned = pruneProjectBuckets(bp, 2);
    EXPECT_EQ(pruned.size(), 2);
    EXPECT_FALSE(pruned.contains("/p/c"));  // largest root evicted on the tie
    EXPECT_TRUE(pruned.contains("/p/a"));
    EXPECT_TRUE(pruned.contains("/p/b"));
    // Pure: same inputs → same output.
    EXPECT_EQ(pruneProjectBuckets(bp, 2), pruned);
}

// prune is a no-op at/under the cap.
TEST(TokensSavedByProject, PruneNoOpUnderCap) {
    QJsonObject bp;
    bp = foldProjectBucket(bp, "/p/x", 10, "2026-07", "2026-07-20T09:35:00", 24);
    EXPECT_EQ(pruneProjectBuckets(bp, 64).size(), 1);
    EXPECT_EQ(pruneProjectBuckets(bp, 0).size(), 1);  // keepProjects<=0 → no-op
}

// The promoted public divisor is usable by symbol (M-A).
TEST(TokensSavedByProject, KCharsPerTokenIsPublic) {
    EXPECT_EQ(TokenUsageEngine::kCharsPerToken, 4);
    EXPECT_EQ(2000 / TokenUsageEngine::kCharsPerToken, 500);
}
