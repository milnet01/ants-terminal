// ANTS-1426 — feature-conformance test for parseBullets's
// blank-line-continuation handling. Behavioural test against
// RoadmapDialog::parseBullets's synthetic fixtures (no real
// ROADMAP.md needed).

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>

// INV-1 — baseline: a bullet with no blank lines parses cleanly.
TEST(roadmap_parser_blank_line_continuation, Inv1BaselineNoBlankLine) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9001] **Baseline bullet.**\n"
        "  Body line one.\n"
        "  Kind: fix.\n"
        "  Source: test.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets[0].id,   QStringLiteral("ANTS-9001"));
    EXPECT_EQ(bullets[0].kind, QStringLiteral("fix"));
}

// INV-2 — blank line inside body, followed by indented Kind:
// continuation — must be parsed (this is the ANTS-1422 case).
TEST(roadmap_parser_blank_line_continuation, Inv2BlankLineThenIndentedCont) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9002] **Blank-line-in-body bullet.**\n"
        "  First sub-block.\n"
        "\n"
        "  Second sub-block after the blank.\n"
        "  Kind: refactor.\n"
        "  Source: test.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets[0].id,   QStringLiteral("ANTS-9002"));
    EXPECT_EQ(bullets[0].kind, QStringLiteral("refactor"))
        << "ANTS-1426 INV-2: parser must walk past the blank "
           "line when the next non-blank line is still indented "
           "continuation, so the Kind: line is reached";
}

// INV-3 — blank line followed by a new top-level bullet still
// terminates the previous bullet's body.
TEST(roadmap_parser_blank_line_continuation, Inv3NewBulletEndsBody) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9003] **First bullet.**\n"
        "  First-bullet body.\n"
        "  Kind: implement.\n"
        "\n"
        "- 📋 [ANTS-9004] **Second bullet.**\n"
        "  Second-bullet body.\n"
        "  Kind: fix.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2)
        << "ANTS-1426 INV-3: a new top-level bullet after a "
           "blank line must NOT be absorbed into the previous "
           "bullet's body";
    EXPECT_EQ(bullets[0].id,   QStringLiteral("ANTS-9003"));
    EXPECT_EQ(bullets[0].kind, QStringLiteral("implement"));
    EXPECT_EQ(bullets[1].id,   QStringLiteral("ANTS-9004"));
    EXPECT_EQ(bullets[1].kind, QStringLiteral("fix"));
}

// INV-4 — blank line followed by a heading also terminates.
TEST(roadmap_parser_blank_line_continuation, Inv4HeadingEndsBody) {
    const QString md = QStringLiteral(
        "## SectionA\n"
        "\n"
        "- 📋 [ANTS-9005] **Bullet in A.**\n"
        "  Body line.\n"
        "  Kind: fix.\n"
        "\n"
        "## SectionB\n"
        "\n"
        "- 📋 [ANTS-9006] **Bullet in B.**\n"
        "  Body line.\n"
        "  Kind: refactor.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_EQ(bullets[0].sectionHeading, QStringLiteral("SectionA"))
        << "ANTS-1426 INV-4: heading after blank-line resets "
           "the section attribution for subsequent bullets";
    EXPECT_EQ(bullets[1].sectionHeading, QStringLiteral("SectionB"));
}

// INV-5 — blank line at EOF terminates cleanly.
TEST(roadmap_parser_blank_line_continuation, Inv5BlankAtEofTerminates) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9007] **EOF-trailing bullet.**\n"
        "  Body line.\n"
        "  Kind: chore.\n"
        "\n"
        "\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets[0].kind, QStringLiteral("chore"));
}
