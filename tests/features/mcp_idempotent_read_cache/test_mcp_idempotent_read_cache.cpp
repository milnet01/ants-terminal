// Feature-conformance test for ANTS-1357 idempotent-read cache.
// See tests/features/mcp_idempotent_read_cache/spec.md.

#include <gtest/gtest.h>

#include "claudeintegration.h"

#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTest>

namespace {

QJsonObject argsA() {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/a");
    return o;
}

QJsonObject argsB() {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/b");
    return o;
}

}  // namespace

// INV-1 — hit returns the same response bytes put.
TEST(McpIdempotentReadCache, HitWithinTtl) {
    ClaudeIntegration ci;
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsA(), QStringLiteral("RESP"));
    const QString hit = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsA());
    EXPECT_EQ(hit, QStringLiteral("RESP"));
    EXPECT_EQ(ci.idempotentReadCacheSizeForTest(), 1);
}

// INV-2 — past 100 ms TTL, tryGet returns empty (miss).
TEST(McpIdempotentReadCache, MissAfterTtl) {
    ClaudeIntegration ci;
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsA(), QStringLiteral("RESP"));
    QTest::qWait(120);
    const QString hit = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsA());
    EXPECT_TRUE(hit.isEmpty()) << "expected miss past TTL";
}

// INV-9 — different args produce different cache keys.
TEST(McpIdempotentReadCache, MissOnDifferentArgs) {
    ClaudeIntegration ci;
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsA(), QStringLiteral("RESP_A"));
    const QString hitB = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsB());
    EXPECT_TRUE(hitB.isEmpty());
    EXPECT_EQ(ci.idempotentReadCacheSizeForTest(), 1);
}

// INV-4 — non-allowlisted tool never inserts.
TEST(McpIdempotentReadCache, NonAllowlistedNotInserted) {
    ClaudeIntegration ci;
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("workspace_search"), argsA(),
        QStringLiteral("RESP"));
    EXPECT_EQ(ci.idempotentReadCacheSizeForTest(), 0);
    const QString hit = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("workspace_search"), argsA());
    EXPECT_TRUE(hit.isEmpty());
}

// INV-3 — cap enforcement + LRU eviction.
TEST(McpIdempotentReadCache, LruEvictionAtCap) {
    ClaudeIntegration ci;
    // Insert 33 entries under one allowlisted tool with distinct args.
    // Cap is 32 (claudeintegration.h kIdempotentReadCacheCap).
    for (int i = 0; i < 33; ++i) {
        QJsonObject o;
        o[QStringLiteral("caller_cwd")] =
            QStringLiteral("/proj/%1").arg(i);
        ci.putIdempotentReadCacheForTest(
            QStringLiteral("tab_list"), o,
            QStringLiteral("R%1").arg(i));
    }
    EXPECT_EQ(ci.idempotentReadCacheSizeForTest(), 32);
    // The first-inserted entry (i=0) must have been evicted.
    QJsonObject zeroArgs;
    zeroArgs[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/0");
    const QString missZero = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), zeroArgs);
    EXPECT_TRUE(missZero.isEmpty()) << "oldest entry must be evicted";
    // The last-inserted (i=32) is present.
    QJsonObject lastArgs;
    lastArgs[QStringLiteral("caller_cwd")] =
        QStringLiteral("/proj/32");
    const QString hitLast = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), lastArgs);
    EXPECT_EQ(hitLast, QStringLiteral("R32"));
}

// INV-5(a) — empty responses are not cached.
TEST(McpIdempotentReadCache, EmptyResponseNotInserted) {
    ClaudeIntegration ci;
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsA(), QString());
    EXPECT_EQ(ci.idempotentReadCacheSizeForTest(), 0);
}

// INV-5(b) — kMcpRcUnavailable literal is not cached.
TEST(McpIdempotentReadCache, RcUnavailableNotInserted) {
    ClaudeIntegration ci;
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), argsA(),
        QString::fromUtf8(ClaudeIntegration::kMcpRcUnavailable));
    EXPECT_EQ(ci.idempotentReadCacheSizeForTest(), 0);
}

// LRU bump: re-hitting an entry moves it to MRU front, so it survives
// later evictions that would have removed it.
TEST(McpIdempotentReadCache, HitBumpsToMru) {
    ClaudeIntegration ci;
    // Insert "anchor" (i=0) first.
    QJsonObject anchor;
    anchor[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/0");
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), anchor,
        QStringLiteral("ANCHOR"));
    // Insert 31 more (total 32 = cap).
    for (int i = 1; i < 32; ++i) {
        QJsonObject o;
        o[QStringLiteral("caller_cwd")] =
            QStringLiteral("/proj/%1").arg(i);
        ci.putIdempotentReadCacheForTest(
            QStringLiteral("tab_list"), o,
            QStringLiteral("R%1").arg(i));
    }
    // Hit the anchor — moves it to MRU front.
    const QString anchorHit = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), anchor);
    EXPECT_EQ(anchorHit, QStringLiteral("ANCHOR"));
    // Insert one more — should evict the LRU (i=1), not the anchor.
    QJsonObject extra;
    extra[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/99");
    ci.putIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), extra,
        QStringLiteral("EXTRA"));
    const QString anchorStillHit =
        ci.tryGetIdempotentReadCacheForTest(
            QStringLiteral("tab_list"), anchor);
    EXPECT_EQ(anchorStillHit, QStringLiteral("ANCHOR"))
        << "MRU bump should have protected anchor from eviction";
    QJsonObject oldest;
    oldest[QStringLiteral("caller_cwd")] = QStringLiteral("/proj/1");
    const QString oldestMiss = ci.tryGetIdempotentReadCacheForTest(
        QStringLiteral("tab_list"), oldest);
    EXPECT_TRUE(oldestMiss.isEmpty())
        << "LRU (after the anchor bump) was i=1, must be evicted";
}
