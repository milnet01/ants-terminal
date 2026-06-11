// ANTS-1438 — feature-conformance test for GFM-adapter bold-ID
// widening. Source-scrape for emission anchors + behavioural test
// against RoadmapDialog::parseBullets with synthetic markdown
// modelled on Vestige's exact bullet shapes.

#include "../../_support/expect.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QString>
#include <QStringList>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {


RoadmapDialog::BulletRecord parseOne(const QString &markdown) {
    const auto bullets = RoadmapDialog::parseBullets(markdown);
    if (bullets.isEmpty()) return {};
    return bullets.first();
}

}  // namespace

// INV-1 — widened regex matches multi-token bold prefixes.
TEST(gfm_adapter_bold_id_multitoken, Inv1MultiTokenBold) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **FW W5 (cont.)** — add a reference-regression spec.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.boldId, QStringLiteral("FW W5 (cont.)"));
    EXPECT_EQ(rec.id, QStringLiteral("FW W5 (cont.)"));
    EXPECT_FALSE(rec.synthetic);
    EXPECT_EQ(rec.format, QStringLiteral("github-task-list"));
}

// INV-2 — ants-style `**Sh4.**` still yields "Sh4" (trailing period
// stripped). Back-compat with multi-prefix ants-style projects.
TEST(gfm_adapter_bold_id_multitoken, Inv2TrailingPeriodStripped) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **Sh4.** — a single-token ants-style bullet.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.boldId, QStringLiteral("Sh4"));
    EXPECT_EQ(rec.id, QStringLiteral("Sh4"));
    EXPECT_FALSE(rec.synthetic);
}

// INV-3 — native ants-v1 bullet (`- ✅ [ANTS-NNNN] **Title.**`)
// untouched; boldId stays empty, id from the [ANTS-NNNN] token.
TEST(gfm_adapter_bold_id_multitoken, Inv3NativeUntouched) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- ✅ [ANTS-1] **Some feature.** ANTS-v1 native bullet.\n");
    const auto rec = parseOne(md);
    EXPECT_TRUE(rec.boldId.isEmpty())
        << "native bullet should not have boldId; got "
        << rec.boldId.toStdString();
    EXPECT_EQ(rec.id, QStringLiteral("ANTS-1"));
    EXPECT_FALSE(rec.synthetic);
    EXPECT_EQ(rec.format, QStringLiteral("ants-v1"));
}

// INV-4 — em-dash separator splits headline from bold-ID.
TEST(gfm_adapter_bold_id_multitoken, Inv4EmDashSplitsHeadline) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **Audit X1** — Phase 3 of the audit self-learning loop.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.boldId, QStringLiteral("Audit X1"));
    EXPECT_EQ(rec.headline,
              QStringLiteral("Phase 3 of the audit self-learning loop."));
}

// INV-5 — no em-dash, falls back to rxBold-based headline path.
// The existing GFM fallback uses the bold text itself as headline.
TEST(gfm_adapter_bold_id_multitoken, Inv5NoSeparatorFallsBack) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **JustBoldNoSeparator** body without separator.\n");
    const auto rec = parseOne(md);
    EXPECT_EQ(rec.boldId, QStringLiteral("JustBoldNoSeparator"));
    // Headline falls back to the rxBold capture (or full line if no
    // bold in body). Either way, NOT empty.
    EXPECT_FALSE(rec.headline.isEmpty());
}

// INV-6 — envelope emission anchors present in remotecontrol.cpp.
TEST(gfm_adapter_bold_id_multitoken, Inv6EnvelopeEmitsBoldId) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_RC_CPP);
    // All three envelope emission sites carry the bold_id field
    // when set. Count occurrences as a proxy — should be ≥ 3
    // (full-file, section-mode, section_index lazy-fill, plus the
    // existing roadmap_log flip path adds a 4th).
    size_t hits = 0;
    size_t pos = 0;
    const std::string needle = "o[\"bold_id\"] = b.boldId";
    while ((pos = cpp.find(needle, pos)) != std::string::npos) {
        ++hits; pos += needle.size();
    }
    expect(hits >= 3,
           "INV-6: bold_id emission landed on all three "
           "cmdRoadmapQuery envelope sites (full-file + "
           "section-mode + section_index lazy-fill)");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — synthetic stays false when boldId is set.
TEST(gfm_adapter_bold_id_multitoken, Inv7SyntheticSuppressed) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **Terrain System** — wrap environment/terrain.\n"
        "- [ ] something with no bold prefix at all.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2);
    // First bullet: bold-ID present → not synthetic.
    EXPECT_EQ(bullets[0].boldId, QStringLiteral("Terrain System"));
    EXPECT_FALSE(bullets[0].synthetic);
    // Second bullet: no bold prefix → synthetic content-hash ID.
    EXPECT_TRUE(bullets[1].boldId.isEmpty());
    EXPECT_TRUE(bullets[1].synthetic);
}

// INV-8 — Vestige fixture round-trips. Exact bullet shapes
// captured from /mnt/Games/Scripts/Linux/3D_Engine/ROADMAP.md
// 2026-05-16.
TEST(gfm_adapter_bold_id_multitoken, Inv8VestigeFixture) {
    const QString md = QStringLiteral(
        "## Phase\n"
        "- [ ] **FW W5 (cont.)** — add a reference-regression spec.\n"
        "- [ ] **Audit X1** — Phase 3 of the audit self-learning loop.\n"
        "- [ ] **Audit/FW X2** — write a unified-pattern doc.\n"
        "- [x] **Terrain System** — wrap environment/terrain.\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 4);

    EXPECT_EQ(bullets[0].boldId, QStringLiteral("FW W5 (cont.)"));
    EXPECT_EQ(bullets[1].boldId, QStringLiteral("Audit X1"));
    EXPECT_EQ(bullets[2].boldId, QStringLiteral("Audit/FW X2"));
    EXPECT_EQ(bullets[3].boldId, QStringLiteral("Terrain System"));

    // None should be synthetic — boldId rescues all of them from
    // the content-hash fallback path.
    for (const auto &b : bullets) {
        EXPECT_FALSE(b.synthetic)
            << "boldId=" << b.boldId.toStdString()
            << " should not be synthetic";
    }

    // Status mapped correctly from checkbox.
    EXPECT_EQ(bullets[0].status, QStringLiteral("📋"));
    EXPECT_EQ(bullets[3].status, QStringLiteral("✅"));
}
