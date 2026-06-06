// ANTS-1987 — feature-conformance test for native-path bold-ID
// extraction. Behavioural against RoadmapDialog::parseBullets over
// synthetic ants-v1 fixtures (no real ROADMAP.md). Reproduces the
// Vestige 2026-06-04 symptom (emoji bold-ID-first bullets read with an
// empty id) and locks in the head-anchored extraction.

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>

namespace {

// An ants-v1 doc mixing the standard `[ID] **headline**` form, the
// bold-ID-first form (`**Cl9.**`), and the bracketed non-dash form
// (`[Cb7]`). The presence of the `[ANTS-0001]` emoji bullet ensures the
// sniffer detects ants-v1 (native path), not GFM/pass-headings.
QString doc() {
    return QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xE2\x9C\x85 [ANTS-0001] **Normal bullet.**\n"
        "- \xF0\x9F\x93\x8B **Cl9.** Short headline here.\n"
        "- \xF0\x9F\x93\x8B [Cb7] **Bracketed headline.**\n"
        "- \xF0\x9F\x9A\xA7 **In-progress thing.** narrator body.\n");
}

const RoadmapDialog::BulletRecord *byHeadlineContains(
        const QVector<RoadmapDialog::BulletRecord> &bs, const char *needle) {
    for (const auto &b : bs) {
        if (b.headline.contains(QString::fromUtf8(needle))) return &b;
    }
    return nullptr;
}

}  // namespace

// INV-1 — a native bold-ID-first bullet gets its id.
TEST(roadmap_query_emoji_bold_id, Inv1BoldIdFirstExtracted) {
    const auto bullets = RoadmapDialog::parseBullets(doc());
    const auto *b = byHeadlineContains(bullets, "Short headline here");
    ASSERT_NE(b, nullptr)
        << "the bold-ID-first bullet must be parsed as a bullet";
    EXPECT_EQ(b->id, QStringLiteral("Cl9"))
        << "INV-1: `- 📋 **Cl9.**` must yield id \"Cl9\" (was empty "
           "before ANTS-1987)";
}

// INV-2 — the standard [PROJ-NNNN] form is unchanged (no regression).
TEST(roadmap_query_emoji_bold_id, Inv2BracketIdUnchanged) {
    const auto bullets = RoadmapDialog::parseBullets(doc());
    const auto *b = byHeadlineContains(bullets, "Normal bullet");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->id, QStringLiteral("ANTS-0001"))
        << "INV-2: the standard `[ANTS-0001] **H**` form keeps its id "
           "(extractBoldId does not fire on a `[`-led head)";
}

// INV-3 — the bracketed non-dash form stays a narrator bullet.
TEST(roadmap_query_emoji_bold_id, Inv3BracketNonDashStaysNarrator) {
    const auto bullets = RoadmapDialog::parseBullets(doc());
    const auto *b = byHeadlineContains(bullets, "Bracketed headline");
    ASSERT_NE(b, nullptr)
        << "the bracketed bullet is still parsed (as a narrator)";
    EXPECT_TRUE(b->id.isEmpty())
        << "INV-3: `[Cb7]` (no -<digits> tail) must NOT be adopted as an "
           "id — the ID token is not widened to dash-less brackets";
}

// INV-4 — a multi-word bold-prose bullet stays a narrator bullet.
TEST(roadmap_query_emoji_bold_id, Inv4MultiWordBoldStaysNarrator) {
    const auto bullets = RoadmapDialog::parseBullets(doc());
    const auto *b = byHeadlineContains(bullets, "In-progress thing");
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(b->id.isEmpty())
        << "INV-4: `- 🚧 **In-progress thing.**` (internal spaces) is not "
           "ID-shaped, so it must NOT be adopted as an id";
}
