// ANTS-1405 — feature-conformance test for the widened stable-ID
// regex in RoadmapDialog::parseBullets. Drives the pure-function
// parser with synthetic markdown fixtures and asserts each invariant
// from spec.md. Also source-scrapes the MCP descriptor for INV-9.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QVector>

ANTS_TEST_SCOPE();

namespace {

RoadmapDialog::BulletRecord parseOne(const QString &markdown) {
    const auto bullets = RoadmapDialog::parseBullets(markdown);
    if (bullets.isEmpty()) return {};
    return bullets.first();
}

}  // namespace

// INV-1 — [ANTS-NNNN] back-compat. Existing Ants bullet shape
// continues to yield id == "ANTS-NNNN" byte-for-byte.
TEST(roadmap_query_external_project_ids, Inv1AntsBackCompat) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- 📋 [ANTS-1234] **Some bullet.** body text.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.id, QStringLiteral("ANTS-1234"));
    EXPECT_EQ(rec.status, QStringLiteral("📋"));
}

// INV-2 — uppercase external project. [MAME-CURATOR-42] yields the
// full literal token.
TEST(roadmap_query_external_project_ids, Inv2UppercaseExternal) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- 📋 [MAME-CURATOR-42] **External project bullet.** body.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.id, QStringLiteral("MAME-CURATOR-42"));
}

// INV-3 — lowercase external project. [mame-curator-7] yields the
// literal lowercase token. This is the exact shape that triggered
// the cross-session report.
TEST(roadmap_query_external_project_ids, Inv3LowercaseExternal) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- 📋 [mame-curator-7] **External project bullet.** body.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.id, QStringLiteral("mame-curator-7"));
}

// INV-4 — digit-leading reject. [42-bad-1] starts with a digit and
// fails the [A-Za-z] anchor; id stays empty.
// ANTS-3492 — the ID grammar accepts a digit-led prefix that CONTAINS a
// letter (3D_E-0042); only a LETTER-FREE token stays a non-id (a date /
// version bracket like [2026-07] is never mistaken for one).
TEST(roadmap_query_external_project_ids, Inv4DigitLedLetterContaining) {
    // Letter-free bracket → still not an id.
    const auto dateRec = parseOne(QStringLiteral(
        "## Section\n"
        "- 📋 [2026-07] **A date, not an ID.** body.\n"));
    EXPECT_TRUE(dateRec.id.isEmpty())
        << "letter-free bracket must not match; got id="
        << dateRec.id.toStdString();
    // Digit-led but letter-containing → now a valid id.
    const auto idRec = parseOne(QStringLiteral(
        "## Section\n"
        "- 📋 [3D_E-0042] **Digit-led project ID.** body.\n"));
    EXPECT_EQ(idRec.id, QStringLiteral("3D_E-0042"));
}

// INV-5 — no-dash bracket id is ADOPTED (superseded by ANTS-1987).
// `[FOO123]` has no `-\d+` tail so the body-wide rxId still rejects it,
// but the ANTS-1987 head-anchored bracket extractor adopts an ID-shaped
// leading bracket (right after the status emoji) so projects that author
// short bare-bracket ids (`[Cl9]`, `[CE18]`, `[FOO123]`) are findable +
// flippable. The original ANTS-1405 contract (this stays empty) was
// reversed by user decision 2026-06-06 — the leading-bracket slot is the
// id slot by convention, and Vestige proved real projects use dash-less
// short ids there.
TEST(roadmap_query_external_project_ids, Inv5NoDashBracketIdAdopted) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- 📋 [FOO123] **Bare-bracket id.** body.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.id, QStringLiteral("FOO123"))
        << "ANTS-1987: a head-anchored ID-shaped bracket is adopted; got id="
        << rec.id.toStdString();
}

// INV-6 — single-letter prefix permitted. [R-5] matches; the "4-6
// letters" rule from § 3.5.1 is documentary, not enforced on the
// read path.
TEST(roadmap_query_external_project_ids, Inv6SingleLetterPrefix) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- 📋 [R-5] **Short prefix bullet.** body.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.id, QStringLiteral("R-5"));
}

// INV-7 — underscore inside prefix permitted. Mirrors the
// rxAntsV1IdBracket charset (which already accepts `_`).
TEST(roadmap_query_external_project_ids, Inv7UnderscoreInPrefix) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- 📋 [my_proj-9] **Underscore-prefix bullet.** body.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.id, QStringLiteral("my_proj-9"));
}

// INV-8 — first-match-wins. A body containing both [ANTS-1234] and
// [mame-curator-7] resolves to the leftmost; parseBullets uses
// rxId.match() (not globalMatch). Sanity-checks back-compat against
// the rare cross-reference case.
TEST(roadmap_query_external_project_ids, Inv8FirstMatchWins) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- 📋 [ANTS-1234] **Self.** see also [mame-curator-7].\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.id, QStringLiteral("ANTS-1234"));
}

// INV-9 — MCP descriptor cites docs/standards/roadmap-format.md and
// gives the [PROJ-NNNN] shape. Source-scrape against
// src/claudeintegration.cpp for the descriptor anchor.
TEST(roadmap_query_external_project_ids, Inv9DescriptorCitesStandard) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(cpp.find("roadmap-format.md") != std::string::npos,
           "INV-9: descriptor cites roadmap-format.md");
    expect(cpp.find("[PROJ-NNNN]") != std::string::npos,
           "INV-9: descriptor shows [PROJ-NNNN] shape");
    expect(cpp.find("ANTS-1405") != std::string::npos,
           "INV-9: descriptor change tagged with ANTS-1405");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — the regression case from the cross-session report: a
// MAME-Curator-style multi-bullet fixture parses with all IDs
// recovered (previously every one was empty).
TEST(roadmap_query_external_project_ids, Inv10MameCuratorFixture) {
    const QString md = QStringLiteral(
        "## P1 — Validation\n"
        "- 📋 [mame-curator-1] **First fixture bullet.** body one.\n"
        "- ✅ [mame-curator-2] **Second fixture bullet.** body two.\n"
        "- 🚧 [mame-curator-3] **Third fixture bullet.** body three.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 3);
    EXPECT_EQ(bullets[0].id, QStringLiteral("mame-curator-1"));
    EXPECT_EQ(bullets[1].id, QStringLiteral("mame-curator-2"));
    EXPECT_EQ(bullets[2].id, QStringLiteral("mame-curator-3"));
    EXPECT_EQ(bullets[0].status, QStringLiteral("📋"));
    EXPECT_EQ(bullets[1].status, QStringLiteral("✅"));
    EXPECT_EQ(bullets[2].status, QStringLiteral("🚧"));
}
