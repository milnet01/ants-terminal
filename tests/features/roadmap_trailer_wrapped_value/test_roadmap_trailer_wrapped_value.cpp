// ANTS-4542 / ANTS-4553 — feature-conformance test: a trailer value that is
// hard-wrapped mid-phrase must continue onto the next line instead of being
// truncated at the wrap. Pre-fix the shared capture stopped at the first
// `.` OR `\n`, so `Lanes: build, ci, tests,` stored three lanes and dropped
// `security.` from the line below — and the renderer then emitted the short
// list terminated with a full stop, which reads as a correct declaration.
// Drives the pure static parseBullets.

#include "../../_support/expect.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QVector>

#include <string>

ANTS_TEST_SCOPE();

namespace {

// 📋 = U+1F4CB (F0 9F 93 8B).
const char *kSeed =
    "# Roadmap\n\n"
    "## Work\n\n"
    // INV-1 — a comma-wrapped lane list.
    "- \xF0\x9F\x93\x8B [ANTS-1000] **Wrapped lane list.**\n"
    "  Dependencies: none. Lanes: build, ci, tests,\n"
    "  security. Kind: chore. Source: planned.\n"
    // INV-2 — a value wrapped mid-parenthetical.
    "- \xF0\x9F\x93\x8B [ANTS-1001] **Wrapped parenthetical.**\n"
    "  Kind: fix. Source: user-2026-04-30 (two reports, same\n"
    "  day).\n"
    // INV-3 — already terminated; must not absorb the line below.
    "- \xF0\x9F\x93\x8B [ANTS-1002] **Terminated value.**\n"
    "  Kind: fix.\n"
    "  This prose is not part of the kind at all.\n"
    // INV-4 — continuation stops at its own full stop.
    "- \xF0\x9F\x93\x8B [ANTS-1003] **Continuation then trailer.**\n"
    "  Lanes: a, b,\n"
    "  c. Kind: chore.\n"
    // INV-5 — a full stop inside a token does not terminate.
    "- \xF0\x9F\x93\x8B [ANTS-1004] **Dotted token.**\n"
    "  Source: finbreak_Ants_MCP_Feedback.md 2026-08-20.\n"
    // INV-6 — a blank line ends the value.
    "- \xF0\x9F\x93\x8B [ANTS-1005] **Paragraph break.**\n"
    "  Lanes: alpha, beta,\n"
    "\n"
    "  gamma is prose in a new paragraph.\n"
    // INV-7 — the ANTS-2058 forms still parse.
    "- \xF0\x9F\x93\x8B [ANTS-1006] **Unwrapped inline.**\n"
    "  Kind: refactor. Lanes: backend tests. Source: seed.\n"
    "- \xF0\x9F\x93\x8B [ANTS-1007] **Line-initial lanes.**\n"
    "  Lanes: solo.\n";

auto seed() {
    return RoadmapDialog::parseBullets(QString::fromUtf8(kSeed));
}

}  // namespace

// INV-1 — a comma-wrapped lane list keeps the lane below the wrap.
TEST(roadmap_trailer_wrapped_value, Inv1WrappedListKeepsTail) {
    const auto b = seed();
    ASSERT_GE(b.size(), 1);
    ASSERT_EQ(b.at(0).lanes.size(), 4)
        << "the wrapped `security` lane was dropped at the line break";
    EXPECT_EQ(b.at(0).lanes.at(0), QStringLiteral("build"));
    EXPECT_EQ(b.at(0).lanes.at(3), QStringLiteral("security"));
    // The continuation must not have swallowed the trailers after it.
    EXPECT_EQ(b.at(0).kind, QStringLiteral("chore"));
}

// INV-2 — a value wrapped mid-parenthetical is rejoined whole.
TEST(roadmap_trailer_wrapped_value, Inv2WrappedParentheticalRejoined) {
    const auto b = seed();
    ASSERT_GE(b.size(), 2);
    EXPECT_EQ(b.at(1).source.toStdString(),
              std::string("user-2026-04-30 (two reports, same day)"))
        << "value was cut at the wrap, losing the closing bracket";
}

// INV-3 — a terminated value does not absorb the following line.
TEST(roadmap_trailer_wrapped_value, Inv3TerminatedValueStops) {
    const auto b = seed();
    ASSERT_GE(b.size(), 3);
    EXPECT_EQ(b.at(2).kind, QStringLiteral("fix"));
}

// INV-4 — the continuation stops at its own full stop, and a trailer
// following on that same line still parses.
TEST(roadmap_trailer_wrapped_value, Inv4ContinuationStopsAtPeriod) {
    const auto b = seed();
    ASSERT_GE(b.size(), 4);
    ASSERT_EQ(b.at(3).lanes.size(), 3);
    EXPECT_EQ(b.at(3).lanes.at(2), QStringLiteral("c"));
    EXPECT_EQ(b.at(3).kind, QStringLiteral("chore"))
        << "the trailer after the continuation must still be read";
}

// INV-5 — a full stop inside a token does not terminate the value.
TEST(roadmap_trailer_wrapped_value, Inv5DottedTokenSurvives) {
    const auto b = seed();
    ASSERT_GE(b.size(), 5);
    EXPECT_EQ(b.at(4).source.toStdString(),
              std::string("finbreak_Ants_MCP_Feedback.md 2026-08-20"));
}

// INV-6 — a blank line ends the value.
TEST(roadmap_trailer_wrapped_value, Inv6BlankLineEndsValue) {
    const auto b = seed();
    ASSERT_GE(b.size(), 6);
    ASSERT_EQ(b.at(5).lanes.size(), 2)
        << "a trailer must not continue across a paragraph break";
    EXPECT_EQ(b.at(5).lanes.at(1), QStringLiteral("beta"));
}

// INV-7 — regression: the unwrapped ANTS-2058 forms are unchanged.
TEST(roadmap_trailer_wrapped_value, Inv7UnwrappedFormsUnchanged) {
    const auto b = seed();
    ASSERT_GE(b.size(), 8);
    EXPECT_EQ(b.at(6).kind, QStringLiteral("refactor"));
    ASSERT_EQ(b.at(6).lanes.size(), 1);
    EXPECT_EQ(b.at(6).lanes.at(0), QStringLiteral("backend tests"));
    EXPECT_EQ(b.at(6).source, QStringLiteral("seed"));
    ASSERT_EQ(b.at(7).lanes.size(), 1);
    EXPECT_EQ(b.at(7).lanes.at(0), QStringLiteral("solo"));
}
