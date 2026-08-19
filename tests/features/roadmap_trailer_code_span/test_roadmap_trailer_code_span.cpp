// Feature-conformance test for ANTS-4504 — a trailer key inside an inline code
// span is a mention, not a declaration.
// Contract and invariant table: tests/features/roadmap_trailer_code_span/spec.md.
//
// Drives RoadmapParse::trailerValuesIn() directly, which is where the guard
// lives, plus one parseBullets() case so the fix is proven at the level a
// consumer actually reads.

#include <gtest/gtest.h>

#include "roadmapparse.h"

#include <QString>

namespace {

// One ants-v1 bullet, with the marker and heading detectRoadmapFormat() needs.
QString doc(const QString &bullet) {
    return QStringLiteral("<!-- ants-roadmap-format: 1 -->\n"
                          "\n"
                          "# Demo — Roadmap\n"
                          "\n"
                          "## Now\n"
                          "\n") + bullet;
}

}  // namespace

// INV-1. The span opens several words ahead of the key, so no fixed-length
// lookbehind can see it. This is the single-line half of ANTS-3808's shape.
TEST(RoadmapTrailerCodeSpan, KeyQuotedLaterInASpanIsNotADeclaration) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  The parser writes `a bullet whose Lanes: core line is an example`\n"
        "  and that example is not a declaration.\n"));
    EXPECT_TRUE(tv.lanes.value.isEmpty())
        << "read a lane out of a code span: " << tv.lanes.value.toStdString();
    EXPECT_TRUE(tv.lanesList.isEmpty());
}

// INV-2. ANTS-3808's real shape: one backticked example carrying TWO keys,
// wrapped by the source across a line break. The second key sits at
// end-of-line preceded by a space, which is what defeats the lookbehinds.
TEST(RoadmapTrailerCodeSpan, KeyInAWrappedSpanIsNotADeclaration) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  The corpus note reads `Kind: fix. Source: in-session-2026-01-01,\n"
        "  found while measuring.` and both keys there are illustration.\n"));
    EXPECT_TRUE(tv.source.value.isEmpty())
        << "read a source out of a wrapped span: "
        << tv.source.value.toStdString();
    EXPECT_TRUE(tv.kind.value.isEmpty())
        << "read a kind out of a wrapped span: " << tv.kind.value.toStdString();
}

// INV-3. The containment claim: quoting a key must not cost the bullet its own
// declaration of that same key, written plainly below.
TEST(RoadmapTrailerCodeSpan, RealDeclarationBesideAQuotedOneStillParses) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  An example of the shape: `Lanes: illustration-only`.\n"
        "  Kind: fix.\n"
        "  Lanes: roadmap.\n"));
    EXPECT_EQ(tv.kind.value, QStringLiteral("fix"));
    ASSERT_EQ(tv.lanesList.size(), 1);
    EXPECT_EQ(tv.lanesList.constFirst(), QStringLiteral("roadmap"));
}

// INV-4. The mask decides WHERE a match is, never WHAT it says: a value
// carrying its own code span is stored verbatim, delimiters included.
TEST(RoadmapTrailerCodeSpan, CapturedValueKeepsItsOwnBackticks) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  Source: in-session-2026-01-01, verifying `rxSource()` on the corpus.\n"));
    EXPECT_EQ(tv.source.value,
              QStringLiteral("in-session-2026-01-01, verifying `rxSource()` "
                             "on the corpus"));
}

// INV-5. A run with no equal-length partner is literal text per CommonMark, so
// one stray backtick must not swallow the tail and silence the real trailers.
TEST(RoadmapTrailerCodeSpan, UnpairedBacktickMasksNothing) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  A sentence with one stray ` backtick in it.\n"
        "  Kind: fix.\n"
        "  Source: in-session-2026-01-01.\n"));
    EXPECT_EQ(tv.kind.value, QStringLiteral("fix"));
    EXPECT_EQ(tv.source.value, QStringLiteral("in-session-2026-01-01"));
}

// INV-5, the multi-backtick half — the pairing is per RUN, so a ``double``
// span is a span, and a lone ``` opens nothing.
TEST(RoadmapTrailerCodeSpan, MultiBacktickRunsPairByLength) {
    const auto quoted = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  Written ``like Kind: doc.`` when the value itself needs a tick.\n"));
    EXPECT_TRUE(quoted.kind.value.isEmpty())
        << "read a kind out of a double-backtick span: "
        << quoted.kind.value.toStdString();

    const auto stray = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  A stray ``` run that closes nowhere.\n"
        "  Kind: fix.\n"));
    EXPECT_EQ(stray.kind.value, QStringLiteral("fix"));
}

// INV-6. A bullet with no backtick at all parses byte-identically — the case
// that reds if the mask is ever applied to the captured text.
TEST(RoadmapTrailerCodeSpan, PlainBulletParsesUnchanged) {
    const auto recs = RoadmapParse::parseBullets(doc(QStringLiteral(
        "- 📋 [DEMO-0001] **An ordinary bullet.**\n"
        "  Some prose.\n"
        "  Kind: implement.\n"
        "  Source: in-session-2026-01-01.\n"
        "  Lanes: core, roadmap.\n")));
    ASSERT_EQ(recs.size(), 1);
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "  Some prose.\n"
        "  Kind: implement.\n"
        "  Source: in-session-2026-01-01.\n"
        "  Lanes: core, roadmap.\n"));
    EXPECT_EQ(tv.kind.value, QStringLiteral("implement"));
    EXPECT_EQ(tv.source.value, QStringLiteral("in-session-2026-01-01"));
    ASSERT_EQ(tv.lanesList.size(), 2);
    EXPECT_EQ(tv.lanesList.constFirst(), QStringLiteral("core"));
}

// End to end: the same wrapped-span body, read the way a consumer reads it.
TEST(RoadmapTrailerCodeSpan, ParseBulletsDoesNotAdoptAQuotedLane) {
    const auto recs = RoadmapParse::parseBullets(doc(QStringLiteral(
        "- 📋 [DEMO-0002] **A bullet documenting the trailer grammar.**\n"
        "  The note reads `Kind: fix. Lanes: packaging,\n"
        "  release.` and that is an illustration.\n"
        "  Kind: doc.\n")));
    ASSERT_EQ(recs.size(), 1);
    EXPECT_TRUE(recs.constFirst().lanes.isEmpty())
        << "adopted a lane the bullet never declared";
}
