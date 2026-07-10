// Feature-conformance test for spec.md (ANTS-3492).
//
// The roadmap ID grammar used to require a LETTER-leading prefix
// (^[A-Za-z]...), so a digit-led project scheme like 3D_E-NNNN
// (Vestige/3D_Engine) was invisible to id-lookup / allocation. The rule
// is relaxed to "prefix CONTAINS at least one letter" — 3D_E-0042 is
// accepted, a letter-free token like 2026-07 is still rejected.

#include "roadmapindex.h"
#include "roadmapfoldin.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

void writeRoadmap(const QString &dir, const QByteArray &body) {
    QFile f(dir + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(body);
}

}  // namespace

// INV-1 / INV-3 — a digit-led, letter-containing id is canonical; every
// pre-existing letter-led id still is.
TEST(RoadmapIdDigitLedPrefix, AcceptsDigitLedLetterContaining) {
    EXPECT_TRUE(RoadmapIndex::isCanonicalId(QStringLiteral("3D_E-0042")));
    EXPECT_TRUE(RoadmapIndex::isCanonicalId(QStringLiteral("3D_E-42")));   // short suffix
    // INV-3 — no regression on the conventional letter-led ids.
    EXPECT_TRUE(RoadmapIndex::isCanonicalId(QStringLiteral("ANTS-0001")));
    EXPECT_TRUE(RoadmapIndex::isCanonicalId(QStringLiteral("RETRO-1234")));
    EXPECT_TRUE(RoadmapIndex::isCanonicalId(QStringLiteral("MYPRJ-42")));
}

// INV-2 — a pure-numeric / letter-free prefix is NOT an id, so a
// date/version bracket like [2026-07] is never mistaken for one.
TEST(RoadmapIdDigitLedPrefix, RejectsLetterFreePrefix) {
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("2026-07")));
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("3-2")));
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("1-1")));
}

// Boundary rejects — first char must be [A-Za-z0-9]; suffix must be digits.
TEST(RoadmapIdDigitLedPrefix, RejectsBoundaryShapes) {
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("_-1")));  // underscore lead
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("A-")));   // no digits
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("-42")));  // empty prefix
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("3D_E")));  // no -NNNN
}

// INV-4 — allocation prefix sniff resolves a digit-led dominant prefix, so
// op:append on a 3D_E- roadmap continues that scheme (not the ANTS default).
TEST(RoadmapIdDigitLedPrefix, SniffsDigitLedPrefix) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeRoadmap(tmp.path(),
        "# ROADMAP\n"
        "- 📋 [3D_E-0022] one\n"
        "- 🚧 [3D_E-0023] two\n"
        "- ✅ [3D_E-0024] three — mentions [UTF-8] in prose\n");
    EXPECT_EQ(RoadmapFoldIn::sniffIdPrefix(tmp.path()),
              QStringLiteral("3D_E"));
    // A letter-free bracket like a date must NOT win the sniff even if it
    // were frequent — it isn't a counter-style id at all.
    QTemporaryDir dated;
    ASSERT_TRUE(dated.isValid());
    writeRoadmap(dated.path(),
        "# ROADMAP\n"
        "- 📋 [3D_E-0030] a (2026-07 sprint)\n"
        "- 📋 [3D_E-0031] b (2026-07 sprint)\n");
    EXPECT_EQ(RoadmapFoldIn::sniffIdPrefix(dated.path()),
              QStringLiteral("3D_E"));
}
