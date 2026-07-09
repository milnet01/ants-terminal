// Feature-conformance test for ANTS-3453 — see spec.md.
//
// Exercises ShapedRunCache's hit/miss accounting, keying, generational
// eviction, and clear() semantics. Shaping needs a font database, so this
// lives in the test_vt GUI bundle (offscreen QApplication from
// bundle_main_gui.cpp).

#include "shapedruncache.h"

#include <QFont>
#include <QString>
#include <QTextLayout>

#include <gtest/gtest.h>

namespace {

QFont monoFont() {
    QFont f(QStringLiteral("monospace"), 11);
    f.setStyleHint(QFont::Monospace);
    f.setKerning(false);
    return f;
}

constexpr int kAscent = 12;  // representative; only consistency matters here

}  // namespace

// INV-1 — a repeated (text, variant) lookup is a hit; misses count distinct
// keys only.
TEST(ShapedRunCache, Inv1RepeatIsHit) {
    ShapedRunCache cache;
    const QFont font = monoFont();
    qreal off = 0;

    cache.layoutFor(QStringLiteral("hello"), 0, font, kAscent, off);
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 0u);

    for (int i = 0; i < 5; ++i)
        cache.layoutFor(QStringLiteral("hello"), 0, font, kAscent, off);
    EXPECT_EQ(cache.misses(), 1u) << "repeat lookups must not re-shape";
    EXPECT_EQ(cache.hits(), 5u);
    EXPECT_EQ(cache.size(), 1u);
}

// INV-2 — a different variant or different text is a distinct entry (miss).
TEST(ShapedRunCache, Inv2VariantAndTextKeyDistinctly) {
    ShapedRunCache cache;
    const QFont font = monoFont();
    qreal off = 0;

    cache.layoutFor(QStringLiteral("word"), 0, font, kAscent, off);  // regular
    cache.layoutFor(QStringLiteral("word"), 1, font, kAscent, off);  // bold
    cache.layoutFor(QStringLiteral("word"), 2, font, kAscent, off);  // italic
    cache.layoutFor(QStringLiteral("other"), 0, font, kAscent, off); // new text
    EXPECT_EQ(cache.misses(), 4u);
    EXPECT_EQ(cache.hits(), 0u);
    EXPECT_EQ(cache.size(), 4u);

    // Re-lookup each distinct key — all hits now.
    cache.layoutFor(QStringLiteral("word"), 0, font, kAscent, off);
    cache.layoutFor(QStringLiteral("word"), 1, font, kAscent, off);
    EXPECT_EQ(cache.hits(), 2u);
    EXPECT_EQ(cache.misses(), 4u);
}

// INV-3 — baselineOff is stable across hit/miss and the layout text
// round-trips.
TEST(ShapedRunCache, Inv3BaselineStableAndTextRoundTrips) {
    ShapedRunCache cache;
    const QFont font = monoFont();

    qreal offMiss = -999;
    QTextLayout *first = cache.layoutFor(QStringLiteral("baseline"), 0, font,
                                         kAscent, offMiss);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->text(), QStringLiteral("baseline"));

    qreal offHit = -999;
    QTextLayout *second = cache.layoutFor(QStringLiteral("baseline"), 0, font,
                                          kAscent, offHit);
    EXPECT_EQ(second, first) << "a hit must return the same cached layout";
    EXPECT_DOUBLE_EQ(offHit, offMiss) << "cached baselineOff must be stable";
}

// INV-4 — generational eviction bounds size to <= 2*capacity; every layout is
// valid; the most-recently-inserted key is a hit on immediate re-lookup.
TEST(ShapedRunCache, Inv4GenerationalEvictionBounded) {
    constexpr std::size_t cap = 4;
    ShapedRunCache cache(cap);
    const QFont font = monoFont();
    qreal off = 0;

    for (int i = 0; i < 100; ++i) {
        QTextLayout *lay = cache.layoutFor(
            QStringLiteral("run%1").arg(i), 0, font, kAscent, off);
        ASSERT_NE(lay, nullptr);
        EXPECT_LE(cache.size(), 2 * cap) << "size exceeded 2*capacity at i=" << i;
    }

    // The last key inserted is still hot — an immediate re-lookup is a hit.
    const std::uint64_t hitsBefore = cache.hits();
    cache.layoutFor(QStringLiteral("run99"), 0, font, kAscent, off);
    EXPECT_EQ(cache.hits(), hitsBefore + 1) << "most-recent key must be retained";

    // A long-evicted early key re-shapes (miss), still returning a valid layout.
    const std::uint64_t missesBefore = cache.misses();
    QTextLayout *evicted = cache.layoutFor(QStringLiteral("run0"), 0, font,
                                           kAscent, off);
    ASSERT_NE(evicted, nullptr);
    EXPECT_EQ(cache.misses(), missesBefore + 1) << "evicted key must re-shape";
}

// INV-5 — clear() empties the cache; next lookup is a miss; lifetime counters
// survive the clear.
TEST(ShapedRunCache, Inv5ClearEmptiesButKeepsCounters) {
    ShapedRunCache cache;
    const QFont font = monoFont();
    qreal off = 0;

    cache.layoutFor(QStringLiteral("a"), 0, font, kAscent, off);
    cache.layoutFor(QStringLiteral("b"), 0, font, kAscent, off);
    cache.layoutFor(QStringLiteral("a"), 0, font, kAscent, off);  // hit
    ASSERT_EQ(cache.size(), 2u);
    const std::uint64_t hits = cache.hits();
    const std::uint64_t misses = cache.misses();

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.hits(), hits) << "clear must not reset lifetime hits";
    EXPECT_EQ(cache.misses(), misses) << "clear must not reset lifetime misses";

    cache.layoutFor(QStringLiteral("a"), 0, font, kAscent, off);  // miss again
    EXPECT_EQ(cache.misses(), misses + 1);
    EXPECT_EQ(cache.size(), 1u);
}
