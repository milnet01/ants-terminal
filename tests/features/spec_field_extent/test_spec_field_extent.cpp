// Feature-conformance test for the markdown header-field extent rule
// (ANTS-3785, covering ANTS-3672). Drives the pure SpecParse::headerField
// transform + parseSpecBody's reader half. The writer half (setStatus) lives
// in tests/features/mcp_spec_log/. See spec.md +
// docs/specs/ANTS-3785-header-field-extent.md.

#include "specparse.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace {

QStringList lines(const char *text) {
    return QString::fromUtf8(text).split(QLatin1Char('\n'));
}

// Byte-for-byte copy of docs/specs/ANTS-1436.md's header. Checked in on
// purpose: binding this expectation to the live spec would break the suite the
// first time anyone flips that spec's status, which this very change enables.
const char *kAnts1436Header =
    "# ANTS-1436 — `roadmap_query` pagination + auto-truncate for large active payloads\n"
    "\n"
    "**Status:** shipped 2026-05-16 (commit f9647bc; `src/paginationengine.{h,cpp}`\n"
    "+ `cmdRoadmapQuery` offset/limit; tests `tests/features/roadmap_query_pagination/`).\n"
    "**Kind:** perf.\n";

// The literal `tools/spec-header-survey.py --values` prints for that file.
const char *kAnts1436Joined =
    "shipped 2026-05-16 (commit f9647bc; `src/paginationengine.{h,cpp}` "
    "+ `cmdRoadmapQuery` offset/limit; tests "
    "`tests/features/roadmap_query_pagination/`).";

// The docs/specs/ANTS-1253.md shape: two field markers on one physical line.
const char *kAnts1253Header =
    "# ANTS-1253 — Consolidate MCP-tool provider registry\n"
    "\n"
    "**Kind:** refactor (no behaviour change). **Lanes:** claudeintegration,\n"
    "mainwindow.\n"
    "\n"
    "## 1. Problem\n";

}  // namespace

// T1 (INV-1) — the extent terminates on each of the four terminators.
TEST(SpecFieldExtent, TerminatesOnBlankLine) {
    const auto e = SpecParse::headerField(
        lines("# T\n\n**Status:** one\ntwo\n\ntail\n"), QStringLiteral("Status"));
    ASSERT_TRUE(e.found());
    EXPECT_EQ(e.lineCount, 2);
    EXPECT_EQ(e.value, QStringLiteral("one two"));
}

TEST(SpecFieldExtent, TerminatesOnNextFieldMarker) {
    const auto e = SpecParse::headerField(
        lines("# T\n\n**Status:** one\ntwo\n**Kind:** fix.\n"),
        QStringLiteral("Status"));
    ASSERT_TRUE(e.found());
    EXPECT_EQ(e.lineCount, 2);
    EXPECT_EQ(e.value, QStringLiteral("one two"));
}

TEST(SpecFieldExtent, TerminatesOnAtxHeading) {
    // No blank separator: a heading may interrupt a paragraph, so it must not
    // be swallowed into the value.
    const auto e = SpecParse::headerField(
        lines("# T\n\n**Status:** one\ntwo\n## 1. Problem\n"),
        QStringLiteral("Status"));
    ASSERT_TRUE(e.found());
    EXPECT_EQ(e.lineCount, 2);
    EXPECT_EQ(e.value, QStringLiteral("one two"));
}

TEST(SpecFieldExtent, TerminatesOnEndOfInput) {
    const auto e = SpecParse::headerField(
        lines("# T\n\n**Status:** one\ntwo"), QStringLiteral("Status"));
    ASSERT_TRUE(e.found());
    EXPECT_EQ(e.lineCount, 2);
    EXPECT_EQ(e.value, QStringLiteral("one two"));
}

// T2 (INV-2) — a bullet-leading continuation belongs to the field, and the
// joined value is exact. Asserting the whole string is what makes the
// separator and the strip testable; a tail-only check survives a join on ""
// or "\n".
TEST(SpecFieldExtent, BulletLeadingContinuationBelongsToField) {
    const auto e = SpecParse::headerField(lines(kAnts1436Header),
                                          QStringLiteral("Status"));
    ASSERT_TRUE(e.found());
    EXPECT_EQ(e.lineCount, 2);
    EXPECT_EQ(e.value, QString::fromUtf8(kAnts1436Joined));
}

// T3 (INV-3) — parseSpecBody returns the whole joined value, not the
// first-line prefix the pre-fix parser returned.
TEST(SpecFieldExtent, ParseSpecBodyReturnsJoinedStatus) {
    const QJsonObject out =
        SpecParse::parseSpecBody(QString::fromUtf8(kAnts1436Header));
    EXPECT_EQ(out.value("status").toString(),
              QString::fromUtf8(kAnts1436Joined));
    EXPECT_NE(out.value("status").toString(),
              QStringLiteral("shipped 2026-05-16 (commit f9647bc; "
                             "`src/paginationengine.{h,cpp}`"))
        << "this is the pre-fix truncation";
    EXPECT_EQ(out.value("kind").toString(), QStringLiteral("perf."));
}

// T4 (INV-8) — Kind obeys the same rule. Synthetic: the corpus's only wrapped
// Kind is also its inline-marker case, and one fixture for two rules proves
// neither.
TEST(SpecFieldExtent, KindWrapsUnderTheSameRule) {
    const auto e = SpecParse::headerField(
        lines("# T\n\n**Kind:** refactor, and a clause that\nwrapped.\n\n"),
        QStringLiteral("Kind"));
    ASSERT_TRUE(e.found());
    EXPECT_EQ(e.lineCount, 2);
    EXPECT_EQ(e.value, QStringLiteral("refactor, and a clause that wrapped."));
}

// T5 (INV-9) — an inline bold colon-run is value text, not a field boundary.
TEST(SpecFieldExtent, InlineMarkerIsValueText) {
    const auto e = SpecParse::headerField(lines(kAnts1253Header),
                                          QStringLiteral("Kind"));
    ASSERT_TRUE(e.found());
    EXPECT_EQ(e.lineCount, 2);
    EXPECT_TRUE(e.value.contains(QStringLiteral("**Lanes:**")))
        << "the inline marker must survive inside the value, not split it";
    EXPECT_EQ(e.value,
              QStringLiteral("refactor (no behaviour change). "
                             "**Lanes:** claudeintegration, mainwindow."));
}

// T6 (INV-10, reader leg) — the search is bounded to the header block, so a
// fenced **Status:** below the first `## ` heading is never matched.
TEST(SpecFieldExtent, SearchIsBoundedToHeaderBlock) {
    const auto e = SpecParse::headerField(
        lines("# T\n"
              "\n"
              "**Kind:** doc.\n"
              "\n"
              "## 3. Header block\n"
              "\n"
              "```\n"
              "**Status:** spec draft (YYYY-MM-DD).\n"
              "```\n"),
        QStringLiteral("Status"));
    EXPECT_FALSE(e.found())
        << "a fenced example below the header block is not this doc's Status";
}

// T7 (INV-6) — one implementation across the two consumers § 2.2 names. The
// POSITIVE half is load-bearing: an absence-only scrape passes against a
// hand-rolled startsWith("**Status:**") that never adopted the shared rule.
// The absence half greps the IDENTIFIERS, because a correct speclog.cpp still
// contains the literal `**Status:**` twice by design (the replacement line and
// the unrecognised_format message).
TEST(SpecFieldExtent, BothConsumersShareOneImplementation) {
    const std::string specLog = ants_test::slurpFile(SRC_SPECLOG_CPP_PATH);
    const std::string specParse = ants_test::slurpFile(SRC_SPECPARSE_CPP_PATH);

    EXPECT_NE(specLog.find("headerField("), std::string::npos)
        << "speclog.cpp must call the shared helper";
    EXPECT_NE(specParse.find("headerField("), std::string::npos)
        << "specparse.cpp must call the shared helper";

    EXPECT_EQ(specLog.find("statusRe"), std::string::npos)
        << "speclog.cpp must carry no field regex of its own";
    EXPECT_EQ(specParse.find("statusRe"), std::string::npos)
        << "specparse.cpp must carry no field regex of its own";
    EXPECT_EQ(specParse.find("kindRe"), std::string::npos)
        << "specparse.cpp must carry no field regex of its own";
}
