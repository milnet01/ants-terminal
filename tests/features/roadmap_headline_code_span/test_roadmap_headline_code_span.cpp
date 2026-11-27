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

// ANTS-6962 — the headline mask pairs code spans by CommonMark's rule, the
// one maskQuotedRegions() uses for trailers: a run of N backticks closes only
// on a run of exactly N. A ``double`` span holding a single backtick is ONE
// span, so the `**` inside it is quoted text and does not close the headline.
TEST(RoadmapHeadlineCodeSpan, DoubleBacktickSpanHoldingOneBacktickIsOneSpan) {
    EXPECT_EQ(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0007] **Quote ``a ** b` `` in text.**\n"
                  "  Kind: implement.\n")),
              QStringLiteral("Quote ``a ** b` `` in text."));
}

// ANTS-6962 — a run with no partner of its own length is literal text and
// opens nothing. Here the ``` run must not pair with the single backtick that
// opens `code` on the next line and hide the headline's closing `**`.
TEST(RoadmapHeadlineCodeSpan, UnpartneredRunOpensNothing) {
    EXPECT_EQ(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0008] **One ``` stray run.**\n"
                  "  Body quotes `code` here.\n"
                  "  Kind: implement.\n")),
              QStringLiteral("One ``` stray run."));
}

// ANTS-6962 — the no-regression half. The mask blanks a span's delimiters to
// spaces, but a backtick beside a `**` is still not whitespace when the bold
// matcher (ANTS-6710) decides whether that `**` opens a nested span. Read off
// the mask, the second `**` would look like it closes, and the headline would
// come out as "Use".
TEST(RoadmapHeadlineCodeSpan, BoldBesideACodeSpanStillNests) {
    EXPECT_EQ(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0009] **Use **`foo`** here.**\n"
                  "  Kind: implement.\n")),
              QStringLiteral("Use **`foo`** here."));
}

// ANTS-6962 — fenced lines are masked as they are for trailers (ANTS-4526): a
// bold run inside a fenced block is a quoted example, never the headline. The
// tilde fence carries no backtick, so nothing masked it before.
TEST(RoadmapHeadlineCodeSpan, BoldInsideAFencedBlockIsNotTheHeadline) {
    EXPECT_NE(headlineOf(QStringLiteral(
                  "- 📋 [DEMO-0010] A headline with no bold run.\n"
                  "  ~~~\n"
                  "  **quoted bold**\n"
                  "  ~~~\n"
                  "  Kind: implement.\n")),
              QStringLiteral("quoted bold"));
}
