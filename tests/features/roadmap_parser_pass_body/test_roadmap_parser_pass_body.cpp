// ANTS-2030 — feature-conformance test for pass-heading bullet body.
// Behavioural test against RoadmapDialog::parseBullets's pass-headings
// dispatch: the body must carry the under-heading prose, not the
// headline. Reproduces RetroDB's include_body no-op.

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>

namespace {

QString twoPassDoc() {
    return QStringLiteral(
        "## Active\n"
        "\n"
        "#### Pass 12.3 (CRITICAL, S) First item\n"
        "\n"
        "- **Status**: in-progress\n"
        "- **Finding**: the first finding text.\n"
        "- **Decision**: do the first thing.\n"
        "\n"
        "#### Pass 12.4 (LOW, S) Second item\n"
        "- **Status**: todo\n"
        "- **Items**: second-item list.\n");
}

}  // namespace

// INV-1 — body carries the under-heading prose and differs from headline.
TEST(roadmap_parser_pass_body, Inv1BodyIsUnderHeadingProse) {
    const auto bullets = RoadmapDialog::parseBullets(twoPassDoc());
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_NE(bullets[0].body, bullets[0].headline)
        << "INV-1: body must not equal headline (the no-op bug)";
    EXPECT_TRUE(bullets[0].body.contains(QStringLiteral("**Finding**")))
        << "INV-1: body carries the Finding prose; got: "
        << bullets[0].body.toStdString();
    EXPECT_TRUE(bullets[0].body.contains(QStringLiteral("**Decision**")))
        << "INV-1: body carries the Decision prose";
}

// INV-2 — body of pass 12.3 does not bleed into pass 12.4's content.
TEST(roadmap_parser_pass_body, Inv2BodyBoundedByNextHeading) {
    const auto bullets = RoadmapDialog::parseBullets(twoPassDoc());
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_FALSE(bullets[0].body.contains(QStringLiteral("**Items**")))
        << "INV-2: first pass body must stop at the next heading; got: "
        << bullets[0].body.toStdString();
    EXPECT_TRUE(bullets[1].body.contains(QStringLiteral("**Items**")))
        << "INV-2: second pass body carries its own content";
}

// INV-3 — leading blank line after the heading is trimmed.
TEST(roadmap_parser_pass_body, Inv3LeadingBlankTrimmed) {
    const auto bullets = RoadmapDialog::parseBullets(twoPassDoc());
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_FALSE(bullets[0].body.startsWith(QChar('\n')))
        << "INV-3: leading blank line stripped from the body";
    EXPECT_TRUE(bullets[0].body.startsWith(QStringLiteral("- **Status**")))
        << "INV-3: body begins at the first content line";
}

// INV-4 — heading with no content beneath falls back to the headline.
// (The doc carries ≥2 `- **Status**:` markers so the format sniffer
// still classifies it as pass-headings; pass 5.1 itself is bare.)
TEST(roadmap_parser_pass_body, Inv4EmptySectionFallsBackToHeadline) {
    const QString md = QStringLiteral(
        "## Active\n"
        "\n"
        "#### Pass 5.1 (LOW, S) Bare heading\n"
        "#### Pass 5.2 (LOW, S) Has content\n"
        "- **Status**: todo\n"
        "\n"
        "#### Pass 5.3 (LOW, S) More content\n"
        "- **Status**: todo\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 3);
    EXPECT_EQ(bullets[0].body, bullets[0].headline)
        << "INV-4: bare heading keeps body == headline (never empty)";
}
