// ANTS-1987 — feature-conformance test for native-path ID extraction
// on emoji-status bullets. Behavioural against
// RoadmapDialog::parseBullets over synthetic fixtures (no real
// ROADMAP.md). Reproduces the Vestige symptom (emoji bullets read with
// an empty id) for BOTH authored shapes:
//   - bold-dotted ID:   `- 📋 **Cl9.** headline`      (2026-06-04 fix)
//   - bare-bracket ID:   `- 📋 [Cl9] **headline**`     (2026-06-06 fix)
// and locks in the head-anchored, ID-shaped, link-guarded extraction.

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>

namespace {

// An ants-v1 doc mixing the standard `[ID] **headline**` form, the
// bold-ID-first form (`**Cl9.**`), the bare-bracket form (`[Cb7]`), a
// markdown-link-led bullet, and a bold-prose narrator. The `[ANTS-0001]`
// emoji bullet makes the sniffer detect ants-v1 (native path).
QString doc() {
    return QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xE2\x9C\x85 [ANTS-0001] **Normal bullet.**\n"
        "- \xF0\x9F\x93\x8B **Cl9.** Short headline here.\n"
        "- \xF0\x9F\x93\x8B [Cb7] **Bracketed headline.**\n"
        "- \xF0\x9F\x93\x8B [docs](https://x) **Link-led headline.**\n"
        "- \xF0\x9F\x9A\xA7 **In-progress thing.** narrator body.\n");
}

// A github-task-list doc — the *actual* Vestige scenario: most items are
// `- [x] **Cl1.** …` checkboxes, but a few active items are authored as
// `- 📋 [Cl9] **headline**` (emoji + bare-bracket id). The checkbox lines
// make detectRoadmapFormat pick "github-task-list"; the emoji bullets
// still route through the native path (gfmHere needs a `[ ]`/`[x]` head).
QString gfmDoc() {
    return QString::fromUtf8(
        "## Slice 17\n"
        "\n"
        "- [x] **Cl1.** Done thing.\n"
        "- [ ] **Cl2.** Todo thing.\n"
        "- \xF0\x9F\x93\x8B [Cl9] **Cloth GPU accelerator.**\n"
        "- \xF0\x9F\x93\x8B [Cl10] **Port-vs-document cloth features.**\n"
        "- \xF0\x9F\x93\x8B [CE18] **Physics spec layer-model drift.**\n");
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
        << "INV-2: the standard `[ANTS-0001] **H**` form keeps its id";
}

// INV-3 — the bare-bracket id form IS adopted (the actual Vestige shape).
// This reverses the pre-2026-06-06 contract, which left `[Cb7]` a narrator
// bullet. The head-anchored, ID-shaped match is a positional signal that
// does NOT widen the body-wide idTokenPattern.
TEST(roadmap_query_emoji_bold_id, Inv3BareBracketIdAdopted) {
    const auto bullets = RoadmapDialog::parseBullets(doc());
    const auto *b = byHeadlineContains(bullets, "Bracketed headline");
    ASSERT_NE(b, nullptr)
        << "the bare-bracket bullet must be parsed as a bullet";
    EXPECT_EQ(b->id, QStringLiteral("Cb7"))
        << "INV-3: `- 📋 [Cb7] **H**` must yield id \"Cb7\" (was empty — "
           "the actual Vestige-reported gap)";
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

// INV-5 — a leading markdown link (`[label](url)`) is NOT adopted as an
// id. The `(?!\\()` link-guard keeps the bracket extraction from eating a
// single-word link label.
TEST(roadmap_query_emoji_bold_id, Inv5MarkdownLinkNotAdopted) {
    const auto bullets = RoadmapDialog::parseBullets(doc());
    const auto *b = byHeadlineContains(bullets, "Link-led headline");
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(b->id.isEmpty())
        << "INV-5: `- 📋 [docs](url) **H**` is a markdown link, not an id";
}

// INV-6 — the faithful Vestige scenario: bare-bracket emoji bullets mixed
// into a github-task-list doc are all found by id.
TEST(roadmap_query_emoji_bold_id, Inv6GfmDocBracketIdsFound) {
    const auto bullets = RoadmapDialog::parseBullets(gfmDoc());
    for (const char *want : {"Cl9", "Cl10", "CE18"}) {
        const QString id = QString::fromUtf8(want);
        bool found = false;
        for (const auto &b : bullets)
            if (b.id == id) { found = true; break; }
        EXPECT_TRUE(found)
            << "INV-6: bracket-id \"" << want << "\" must be indexed in a "
               "github-task-list doc (emoji bullet routes native path)";
    }
}

// ANTS-4378 — the UNDOTTED bold-id form `**LOTTO-0010** headline` keeps its
// headline. The dotted `**Cl9.** headline` form above was already handled;
// this one was not, because the branch that recovers the prose after a
// bold-id token tested `h == boldId + "."` and nothing else. So on a roadmap
// that migrated id format mid-life, headline_only returned the bare id as the
// headline for every bullet in the older shape — 7 of 11 active ones in
// LottoTracker's case.
//
// It cost real duplicated work: LOTTO-0029 was filed as a new idea when
// LOTTO-0010 already covered it, and the duplication was invisible in the
// listing because LOTTO-0010 rendered as the string "LOTTO-0010". Finding a
// duplicate is exactly what that mode should answer.
TEST(roadmap_query_emoji_bold_id, Ants4378UndottedBoldIdKeepsItsHeadline) {
    const QString mixed = QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xE2\x9C\x85 [LOTTO-0001] **A bracketed bullet.**\n"
        "- \xF0\x9F\x93\x8B **LOTTO-0010** Read the payout SMSes and "
        "reconcile them against computed wins.\n"
        "- \xF0\x9F\x93\x8B **LOTTO-0011.** A dotted sibling, already handled.\n");
    const auto bullets = RoadmapDialog::parseBullets(mixed);

    const auto *undotted = byHeadlineContains(bullets, "payout SMSes");
    ASSERT_NE(undotted, nullptr)
        << "the undotted bold-id bullet's headline must be its PROSE, not its "
           "id — a list of bare ids cannot be chosen from";
    EXPECT_EQ(undotted->id, QStringLiteral("LOTTO-0010"));
    EXPECT_FALSE(undotted->headline.startsWith(QStringLiteral("LOTTO-0010")))
        << "headline was: " << undotted->headline.toStdString();

    // Controls — neither sibling shape may regress.
    const auto *dotted = byHeadlineContains(bullets, "dotted sibling");
    ASSERT_NE(dotted, nullptr);
    EXPECT_EQ(dotted->id, QStringLiteral("LOTTO-0011"));
    const auto *bracket = byHeadlineContains(bullets, "bracketed bullet");
    ASSERT_NE(bracket, nullptr);
    EXPECT_EQ(bracket->id, QStringLiteral("LOTTO-0001"));
}
