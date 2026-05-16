// ANTS-1428 Tier 1 — read-side adapter for GFM-task-list
// roadmaps. Behavioural tests against
// `RoadmapDialog::parseBullets`.

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QString>

// INV-1 — adapter engages on a marker-less GFM file.
TEST(AdapterReadGfm, EngagesOnGfmShape) {
    const QString md = QStringLiteral(
        "# Vestige Roadmap\n\n"
        "## Phase 11A\n"
        "- [x] **Sh4.** Shipped item\n"
        "- [ ] **Ed1.** Planned item\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_EQ(bullets[0].format, QStringLiteral("github-task-list"));
    EXPECT_EQ(bullets[0].status, QStringLiteral("✅"));
    EXPECT_EQ(bullets[1].status, QStringLiteral("📋"));
}

// INV-1 — adapter does NOT engage when the format marker is
// present, even if the file contains GFM-looking bullets.
TEST(AdapterReadGfm, DoesNotEngageWithMarker) {
    const QString md = QStringLiteral(
        "<!-- ants-roadmap-format: 1 -->\n"
        "## Section\n"
        "- ✅ [ANTS-0001] **Native bullet**\n"
        // The line below would match GFM if the adapter were on
        // — it's here to prove the marker beats content detection.
        "- [ ] This is body text inside an Ants-format file\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets[0].format, QStringLiteral("ants-v1"));
    EXPECT_EQ(bullets[0].id,     QStringLiteral("ANTS-0001"));
}

// INV-2 — bold-ID preservation across multi-prefix projects.
TEST(AdapterReadGfm, MultiPrefixBoldIdsPreserved) {
    const QString md = QStringLiteral(
        "## Phase 11A\n"
        "- [x] **Sh4.** Shader work\n"
        "- [ ] **Ed1.** Editor work\n"
        "- [ ] **VEST-0042.** Cross-cutting fix\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 3);
    EXPECT_EQ(bullets[0].id,        QStringLiteral("Sh4"));
    EXPECT_FALSE(bullets[0].synthetic);
    EXPECT_EQ(bullets[1].id,        QStringLiteral("Ed1"));
    EXPECT_FALSE(bullets[1].synthetic);
    EXPECT_EQ(bullets[2].id,        QStringLiteral("VEST-0042"));
    EXPECT_FALSE(bullets[2].synthetic);
}

// INV-3 — synthetic-ID stability across line reorders.
TEST(AdapterReadGfm, SyntheticIdStableAcrossReorder) {
    const QString mdA = QStringLiteral(
        "## Section\n"
        "- [ ] First headline goes here\n"
        "- [ ] Second headline goes here\n");
    const QString mdB = QStringLiteral(
        "## Section\n"
        "- [ ] Second headline goes here\n"
        "- [ ] First headline goes here\n");
    const auto a = RoadmapDialog::parseBullets(mdA);
    const auto b = RoadmapDialog::parseBullets(mdB);
    ASSERT_EQ(a.size(), 2);
    ASSERT_EQ(b.size(), 2);
    EXPECT_TRUE(a[0].synthetic);
    EXPECT_TRUE(a[1].synthetic);
    // Reorder: a[0] (First) should match b[1] (First).
    EXPECT_EQ(a[0].id, b[1].id);
    EXPECT_EQ(a[1].id, b[0].id);
    // Synthetic IDs are 10-char base36.
    EXPECT_EQ(a[0].id.size(), 10);
}

// INV-3 — no two distinct headlines collide on a small fixture.
TEST(AdapterReadGfm, SyntheticIdsDistinctOnSmallFixture) {
    QString md = QStringLiteral("## Section\n");
    constexpr int kN = 50;
    for (int i = 0; i < kN; ++i) {
        md.append(QStringLiteral("- [ ] Distinct headline number %1\n").arg(i));
    }
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), kN);
    QSet<QString> seen;
    for (const auto &b : bullets) {
        ASSERT_TRUE(b.synthetic);
        EXPECT_FALSE(seen.contains(b.id))
            << "Synthetic ID collision at headline: "
            << b.headline.toUtf8().constData();
        seen.insert(b.id);
    }
    EXPECT_EQ(seen.size(), kN);
}

// INV-4 — inline emoji wins, including contradictory case
// `- [x] 📋` → 📋.
TEST(AdapterReadGfm, InlineEmojiWinsOverCheckbox) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] 🚧 **Sh1.** In-progress override\n"
        "- [x] 📋 **Sh2.** Done checkbox + planned emoji\n"
        "- [x] 💭 **Sh3.** Done checkbox + deferred emoji\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 3);
    EXPECT_EQ(bullets[0].status, QStringLiteral("🚧"));
    EXPECT_EQ(bullets[1].status, QStringLiteral("📋"));
    EXPECT_EQ(bullets[2].status, QStringLiteral("💭"));
}

// INV-5 — section heading marked (COMPLETE) causes enclosed
// planned bullets to inherit ✅ unless overridden.
TEST(AdapterReadGfm, SectionCompletionInherits) {
    const QString md = QStringLiteral(
        "## Phase 10 (COMPLETE)\n"
        "- [ ] **Sh1.** Inherits done\n"
        "- [ ] 🚧 **Sh2.** Inline override sticks\n"
        "## Phase 11A\n"
        "- [ ] **Sh3.** Stays planned (no marker)\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 3);
    EXPECT_EQ(bullets[0].status, QStringLiteral("✅"));
    EXPECT_EQ(bullets[1].status, QStringLiteral("🚧"));
    EXPECT_EQ(bullets[2].status, QStringLiteral("📋"));
}

// INV-5 — case insensitivity on heading marker.
TEST(AdapterReadGfm, SectionCompletionCaseInsensitive) {
    const QString md = QStringLiteral(
        "## Phase 10 (Complete)\n"
        "- [ ] **Sh1.** Inherits done lowercase mixed\n"
        "## Phase 11A - done\n"
        "- [ ] **Sh2.** Inherits via -done variant\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_EQ(bullets[0].status, QStringLiteral("✅"));
    EXPECT_EQ(bullets[1].status, QStringLiteral("✅"));
}

// Native path regression — make sure the existing ants-v1
// behaviour still produces full BulletRecord fields with
// format == "ants-v1" (default).
TEST(AdapterReadGfm, NativePathStillWorks) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- ✅ [ANTS-0001] **Native bullet**\n"
        "  Kind: fix.\n"
        "  Lanes: parser, dialog.\n"
        "  Layman: nothing fancy.\n"
        "  Source: test.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 1);
    const auto &b = bullets[0];
    EXPECT_EQ(b.id,       QStringLiteral("ANTS-0001"));
    EXPECT_EQ(b.status,   QStringLiteral("✅"));
    EXPECT_EQ(b.kind,     QStringLiteral("fix"));
    EXPECT_EQ(b.lanes.size(), 2);
    EXPECT_EQ(b.layman,   QStringLiteral("nothing fancy"));
    EXPECT_EQ(b.format,   QStringLiteral("ants-v1"));
    EXPECT_FALSE(b.synthetic);
    EXPECT_TRUE(b.anchor.isEmpty());
}

// Caret anchor extraction — the locator handle for Tier 2.
TEST(AdapterReadGfm, CaretAnchorExtracted) {
    const QString md = QStringLiteral(
        "## Phase 11A\n"
        "- [x] **Sh4.** Shipped item ^vest-0042\n"
        "- [ ] Headline without bold-ID ^vest-0043\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_EQ(bullets[0].anchor, QStringLiteral("vest-0042"));
    EXPECT_EQ(bullets[1].anchor, QStringLiteral("vest-0043"));
}
