// Feature-conformance test for ANTS-4066 — a bold marker inside backticks is a
// literal code span, not emphasis, so it must not terminate a bold headline.
// Contract and case table: tests/features/roadmap_headline_code_span/spec.md.
//
// Drives RoadmapParse::parseBullets() on hand-written markdown, which is the
// level the defect lives at: the truncation happens in the matcher, before any
// store or migration is involved.

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

QString headlineOf(const QString &bullet) {
    const auto recs = RoadmapParse::parseBullets(doc(bullet));
    if (recs.isEmpty())
        return QStringLiteral("<no bullet parsed>");
    return recs.constFirst().headline;
}

}  // namespace

// The ANTS-1702 shape: a C signature quoted in a code span. The two asterisks
// are pointer-to-pointer syntax, and the parser used to stop dead on them.
TEST(RoadmapHeadlineCodeSpan, CSignatureInBackticksSurvives) {
    const QString h = headlineOf(QStringLiteral(
        "- ✅ [DEMO-0001] **`-Wunused-parameter` on `runMain(int argc, char "
        "**argv)` in feature-test bundles.**\n"
        "  Kind: fix.\n"));
    EXPECT_EQ(h, QStringLiteral(
        "`-Wunused-parameter` on `runMain(int argc, char **argv)` in "
        "feature-test bundles."))
        << "the headline was cut at the `**` inside the code span";
}

// A bullet ABOUT the roadmap format has to quote the trailer key it discusses.
TEST(RoadmapHeadlineCodeSpan, QuotedBoldMarkerSurvives) {
    const QString h = headlineOf(QStringLiteral(
        "- 📋 [DEMO-0002] **The `**Layman:**` trailer renders once, not twice.**\n"
        "  Kind: fix.\n"));
    EXPECT_EQ(h, QStringLiteral(
        "The `**Layman:**` trailer renders once, not twice."));
}

// The containment claim, and the reason the fix is safe to ship: an ordinary
// bullet must parse byte-identically. This is the case that fails if the mask
// is applied to the captured text rather than only to the matching.
TEST(RoadmapHeadlineCodeSpan, PlainBoldHeadlineUnchanged) {
    EXPECT_EQ(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0003] **An ordinary headline.**\n"
                  "  Kind: implement.\n")),
              QStringLiteral("An ordinary headline."));
    // A code span carrying no asterisk is masked to itself.
    EXPECT_EQ(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0004] **A headline quoting `rxKind()` plainly.**\n"
                  "  Kind: implement.\n")),
              QStringLiteral("A headline quoting `rxKind()` plainly."));
}

// A real bold run that OPENS after a code span must still close where it does
// today — the mask must not shift any offset.
TEST(RoadmapHeadlineCodeSpan, RealBoldAfterACodeSpanStillCloses) {
    EXPECT_EQ(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0005] **`a` then `b` then done.**\n"
                  "  Kind: implement.\n")),
              QStringLiteral("`a` then `b` then done."));
}

// An unterminated backtick must not mask to end-of-string: doing so would hide
// the headline's own closing `**` and lose the headline entirely, which is
// worse than the truncation this fix removes.
TEST(RoadmapHeadlineCodeSpan, UnterminatedBacktickDoesNotSwallowTheHeadline) {
    EXPECT_EQ(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0006] **A headline with one stray ` backtick.**\n"
                  "  Kind: implement.\n")),
              QStringLiteral("A headline with one stray ` backtick."));
}
