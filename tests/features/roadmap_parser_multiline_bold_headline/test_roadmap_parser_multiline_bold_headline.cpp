// ANTS-1561 — feature-conformance test for parseBullets capturing a
// bold headline that wraps across two source lines. Pre-fix, rxBold
// used default options (no DotMatchesEverythingOption), so `.+?` did
// not span `\n` and the regex skipped the headline span entirely,
// falling through to the next `**...**` pair — usually
// `**Layman:**`, which became the bullet's headline.

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QChar>
#include <QString>
#include <QStringLiteral>

namespace {
// Mirror of remotecontrol.cpp's anon-namespace `rcHeadlineOneline`
// — the test asserts the post-collapse one-line form that callers
// actually see on the wire.
QString collapseToOneLine(const QString &headline) {
    if (headline.isEmpty()) return QString();
    QString s;
    s.reserve(headline.size());
    bool prevSpace = false;
    for (QChar c : headline) {
        const bool isWs = c.isSpace() || c == QChar('\n') ||
                          c == QChar('\r') || c == QChar('\t');
        if (isWs) {
            if (!prevSpace) s.append(QChar(' '));
            prevSpace = true;
        } else {
            s.append(c);
            prevSpace = false;
        }
    }
    return s.trimmed();
}
}  // namespace

// INV-1 — single-line bold headline parses unchanged (no regression).
TEST(roadmap_parser_multiline_bold_headline, Inv1SingleLineBaseline) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9101] **Single-line baseline headline.**\n"
        "  Body line.\n"
        "  **Layman:** body in plain words.\n"
        "  Kind: fix.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets[0].headline,
              QStringLiteral("Single-line baseline headline."));
    EXPECT_EQ(collapseToOneLine(bullets[0].headline),
              QStringLiteral("Single-line baseline headline."));
}

// INV-2 — two-line soft-wrapped bold headline is captured in full.
// `headline` carries the raw text (with the embedded `\n` from the
// body collection); the on-the-wire one-line form collapses to a
// single space.
TEST(roadmap_parser_multiline_bold_headline, Inv2TwoLineCaptured) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9102] **`cold_eyes_partition` spec-lane detection\n"
        "  hardcoded to `ANTS-NNNN.md` filename shape.**\n"
        "  Body sentence.\n"
        "  **Layman:** plain-words body.\n"
        "  Kind: fix.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    const QString oneline = collapseToOneLine(bullets[0].headline);
    EXPECT_EQ(oneline,
              QStringLiteral("`cold_eyes_partition` spec-lane detection "
                             "hardcoded to `ANTS-NNNN.md` filename shape."))
        << "ANTS-1561 INV-2: rxBold must capture across the `\\n` so "
           "the full two-line headline survives";
}

// INV-3 — the regression case. Pre-fix, this bullet's headline came
// back as "Layman:" because rxBold skipped the multi-line span and
// matched the next `**Layman:**` pair.
TEST(roadmap_parser_multiline_bold_headline, Inv3LaymanNotHeadline) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9103] **Cold-eyes partition `partition.json` override\n"
        "  schema undocumented + silent fallback on malformed.**\n"
        "  Body sentence.\n"
        "  **Layman:** plain-words body.\n"
        "  Kind: fix.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    const QString oneline = collapseToOneLine(bullets[0].headline);
    EXPECT_NE(oneline, QStringLiteral("Layman:"))
        << "ANTS-1561 INV-3: regression — multi-line headline must "
           "NOT fall through to the **Layman:** bold token";
    EXPECT_TRUE(oneline.startsWith(QStringLiteral(
        "Cold-eyes partition `partition.json` override")))
        << "Actual headline_oneline: " << oneline.toStdString();
}

// INV-4 — the 120-char truncation cap still applies to a long
// multi-line headline (existing behaviour preserved).
TEST(roadmap_parser_multiline_bold_headline, Inv4HeadlineCapApplies) {
    const QString md = QStringLiteral(
        "## Section\n"
        "\n"
        "- 📋 [ANTS-9104] **Lorem ipsum dolor sit amet consectetur "
        "adipiscing elit sed do eiusmod tempor incididunt ut\n"
        "  labore et dolore magna aliqua ut enim ad minim veniam "
        "quis nostrud exercitation ullamco laboris.**\n"
        "  Body sentence.\n"
        "  Kind: chore.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    const QString oneline = collapseToOneLine(bullets[0].headline);
    ASSERT_FALSE(oneline.isEmpty());
    EXPECT_LE(oneline.size(), 121)
        << "ANTS-1561 INV-4: long multi-line headline still bounded "
           "by the 120-char truncation cap + ellipsis";
    EXPECT_TRUE(oneline.endsWith(QChar(0x2026)))
        << "Truncation ellipsis present";
}
