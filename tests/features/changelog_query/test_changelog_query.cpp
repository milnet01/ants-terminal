// ANTS-3533 — feature-conformance test for the changelog_query MCP tool.
// Behavioural on the pure ChangelogQuery::parse parser (the net-new logic);
// source-scrape for the handler registration + allowlist wiring.

#include "../../_support/expect.h"
#include "changelogquery.h"
#include "changeloglog.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QString>
#include <QStringLiteral>

#include <string>

ANTS_TEST_SCOPE();

using ChangelogQuery::Entry;
using ChangelogQuery::ParseResult;

namespace {

const QString kPrefix = QStringLiteral("ANTS");

// Find the first entry whose text contains `needle`.
const Entry *findEntry(const ParseResult &r, const char *needle) {
    for (const Entry &e : r.entries)
        if (e.text.contains(QLatin1String(needle))) return &e;
    return nullptr;
}

}  // namespace

// INV-2 — headings, categories, bullets, dates parsed; version_index rollup.
TEST(ChangelogQueryParse, BasicStructureAndCounts) {
    // fromUtf8 so the em-dash bytes decode to U+2014 (a QStringLiteral
    // would mis-decode them to three Latin-1 chars).
    const QString md = QString::fromUtf8(
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "- **New thing.** (ANTS-1000)\n"
        "  A layman line.\n\n"
        "### Fixed\n\n"
        "- **A fix.** (ANTS-1001)\n\n"
        "## [0.7.99] \xE2\x80\x94 2026-07-17\n\n"
        "### Fixed\n\n"
        "- **An older fix.** (ANTS-0999)\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);

    ASSERT_EQ(r.versions.size(), 2);
    EXPECT_EQ(r.versions[0].version, QStringLiteral("Unreleased"));
    EXPECT_TRUE(r.versions[0].unreleased);
    EXPECT_EQ(r.versions[0].date, QString());
    EXPECT_EQ(r.versions[0].entry_count, 2);
    // canonical order: Added before Fixed; zero-count omitted.
    ASSERT_EQ(r.versions[0].categories.size(), 2);
    EXPECT_EQ(r.versions[0].categories[0].first, QStringLiteral("Added"));
    EXPECT_EQ(r.versions[0].categories[1].first, QStringLiteral("Fixed"));

    // Dated version: em-dash separator stripped, ISO date captured.
    EXPECT_EQ(r.versions[1].version, QStringLiteral("0.7.99"));
    EXPECT_FALSE(r.versions[1].unreleased);
    EXPECT_EQ(r.versions[1].date, QStringLiteral("2026-07-17"));

    ASSERT_EQ(r.entries.size(), 3);
    const Entry *added = findEntry(r, "New thing");
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->category, QStringLiteral("Added"));
    EXPECT_EQ(added->version, QStringLiteral("Unreleased"));
    EXPECT_TRUE(added->body.contains(QLatin1String("A layman line")));
    ASSERT_EQ(added->ids.size(), 1);
    EXPECT_EQ(added->ids[0], QStringLiteral("ANTS-1000"));
}

// INV-3 — mixed parenthetical + unparenthesised non-id token: extract the
// real id only. This is the live CHANGELOG.md:6878 case.
TEST(ChangelogQueryParse, MixedParentheticalExcludesNonIds) {
    const QString md = QStringLiteral(
        "## [Unreleased]\n\n"
        "### Fixed\n\n"
        "- **UTF-8 C1 byte strip in filterControlChars (ANTS-1335,\n"
        "  lane-2 M2 deferral).** The rc/MCP byte filter did a thing.\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);
    ASSERT_EQ(r.entries.size(), 1);
    // Only ANTS-1335 — NOT UTF-8 / lane-2 / M2.
    ASSERT_EQ(r.entries[0].ids.size(), 1);
    EXPECT_EQ(r.entries[0].ids[0], QStringLiteral("ANTS-1335"));
}

// INV-3 — a parenthesised id-shaped non-id (SHA-256) with prefix ≠ P → no id.
TEST(ChangelogQueryParse, ShaLikeTokensNotExtracted) {
    const QString md = QStringLiteral(
        "## [Unreleased]\n\n"
        "### Security\n\n"
        "- **Hash upgrade to SHA-256 (base-64) and UTF-16.** no real id here.\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);
    ASSERT_EQ(r.entries.size(), 1);
    EXPECT_TRUE(r.entries[0].ids.isEmpty());
}

// INV-3 — multiple + mid-bold id placements.
TEST(ChangelogQueryParse, MultipleAndMidBoldIds) {
    const QString md = QStringLiteral(
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "- **Two ids here.** (ANTS-3375, ANTS-3493)\n\n"
        "- **Mid-bold id (ANTS-2169).** and more.\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);
    ASSERT_EQ(r.entries.size(), 2);
    ASSERT_EQ(r.entries[0].ids.size(), 2);
    EXPECT_EQ(r.entries[0].ids[0], QStringLiteral("ANTS-3375"));
    EXPECT_EQ(r.entries[0].ids[1], QStringLiteral("ANTS-3493"));
    ASSERT_EQ(r.entries[1].ids.size(), 1);
    EXPECT_EQ(r.entries[1].ids[0], QStringLiteral("ANTS-2169"));
}

// INV-2 — `unreleased` is driven by the version TOKEN, not the date text.
TEST(ChangelogQueryParse, UnreleasedFlagFromTokenNotDate) {
    const QString md = QString::fromUtf8(
        "## [0.7.100] \xE2\x80\x94 unreleased (Patron RC preview)\n\n"
        "### Added\n\n"
        "- **A thing.** (ANTS-3572)\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);
    ASSERT_EQ(r.versions.size(), 1);
    EXPECT_EQ(r.versions[0].version, QStringLiteral("0.7.100"));
    EXPECT_FALSE(r.versions[0].unreleased);
    EXPECT_EQ(r.versions[0].date, QStringLiteral("unreleased (Patron RC preview)"));
}

// INV-2 — a fenced block suppresses structure; a column-0 `## [` inside an
// unterminated fence is a hard reset that starts a new version.
TEST(ChangelogQueryParse, FenceSuppressionAndHardReset) {
    const QString md = QString::fromUtf8(
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "- **Has a code block.** (ANTS-2000)\n"
        "  ```\n"
        "  ## [9.9.9] not a version\n"
        "  - not a bullet\n"
        "  ```\n"
        "  trailing body.\n\n"
        "## [0.1.0] \xE2\x80\x94 2026-01-01\n\n"
        "### Fixed\n\n"
        "- **Real old fix.** (ANTS-0001)\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);
    // The fenced fake heading/bullet must NOT create a version/entry.
    ASSERT_EQ(r.versions.size(), 2);
    EXPECT_EQ(r.versions[0].version, QStringLiteral("Unreleased"));
    EXPECT_EQ(r.versions[1].version, QStringLiteral("0.1.0"));
    ASSERT_EQ(r.entries.size(), 2);  // not 3 — the fenced "- not a bullet" is body
}

// §3 — non-canonical `### ` resets category; its bullet is skipped.
TEST(ChangelogQueryParse, NonCanonicalCategoryResets) {
    const QString md = QStringLiteral(
        "## [Unreleased]\n\n"
        "### Added\n\n"
        "- **Real added.** (ANTS-3000)\n\n"
        "### Notes\n\n"
        "- **Not a real entry.** (ANTS-3001)\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);
    ASSERT_EQ(r.entries.size(), 1);
    EXPECT_EQ(r.entries[0].ids[0], QStringLiteral("ANTS-3000"));
}

// §3 degenerate — a bullet before any version heading is skipped; empty input.
TEST(ChangelogQueryParse, DegenerateInputs) {
    const ParseResult empty = ChangelogQuery::parse(QString(), kPrefix);
    EXPECT_TRUE(empty.entries.isEmpty());
    EXPECT_TRUE(empty.versions.isEmpty());

    const QString md = QStringLiteral(
        "# Changelog\n\n"
        "- **Orphan bullet before any version.** (ANTS-4000)\n");
    const ParseResult r = ChangelogQuery::parse(md, kPrefix);
    EXPECT_TRUE(r.entries.isEmpty());
}

// ---- Wiring source-scrape (INV-1 / INV-6 / INV-9) ----

TEST(ChangelogQueryWiring, RegisteredAndAllowlisted) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    const std::string mwsq = ants_test::squashWhitespace(mw);
    // Registered verbatim-forward under CallerCwdContract::Required.
    EXPECT_NE(mwsq.find("registerToolProvider(\"changelog_query\""), std::string::npos);
    EXPECT_NE(mwsq.find("rcDelegate(&RemoteControl::cmdChangelogQuery)"),
              std::string::npos);

    // Handler exists.
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("RemoteControl::cmdChangelogQuery"), std::string::npos);

    // Four opt-in allowlists.
    const std::string proj = ants_test::slurpFile(SRC_MCPPROJECTION_CPP_PATH);
    ASSERT_FALSE(proj.empty());
    const std::string fieldBody =
        ants_test::slurpFunctionBody(proj, "isFieldProjectionTool");
    const std::string offloadBody =
        ants_test::slurpFunctionBody(proj, "isOffloadEligible");
    EXPECT_NE(fieldBody.find("changelog_query"), std::string::npos);
    EXPECT_NE(offloadBody.find("changelog_query"), std::string::npos);

    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const std::string etagBody =
        ants_test::slurpFunctionBody(ci, "ClaudeIntegration::isEtagSupportedTool");
    const std::string contractBody =
        ants_test::slurpFunctionBody(ci, "ClaudeIntegration::callerCwdContractFor");
    EXPECT_NE(etagBody.find("changelog_query"), std::string::npos);
    EXPECT_NE(contractBody.find("changelog_query"), std::string::npos);
}

// The category list is public (hoisted for the bad_category echo).
TEST(ChangelogQueryWiring, CanonicalCategoriesPublic) {
    const QStringList &cats = ChangelogLog::canonicalCategories();
    EXPECT_EQ(cats.size(), 6);
    EXPECT_EQ(cats.first(), QStringLiteral("Added"));
    EXPECT_TRUE(cats.contains(QStringLiteral("Security")));
}
