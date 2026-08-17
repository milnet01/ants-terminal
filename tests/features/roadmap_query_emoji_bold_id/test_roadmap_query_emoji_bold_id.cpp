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

// ANTS-4417 — the `**ID** plain headline` shape keeps its headline even when
// the bullet HAS A CONTINUATION BODY CARRYING BOLD, and the recorded strip
// boundary stays on the head line.
//
// Why this is a separate case from ANTS-4378 above, rather than an assertion
// added to it: that fixture is three single-line bullets with no continuation
// body at all. With no bold run anywhere after the bold-id, the next-bold
// search never matches and the prose fallback fires — the correct answer, by
// the only route the fixture can reach. The defect needs a second bold run
// LATER IN THE BODY to appear, so the fixture could not express the breach it
// was written to catch, and the search escaping the head line shipped green.
//
// Measured on the reporting project (2026-08-17): 18 of 31 bullets returned a
// body fragment as the headline — "already received", "empty body",
// "fixed 2026-08-02." — each reading as a statement ABOUT the item.
//
// The second assertion is the half nobody reported. `headlineEnd` is
// ANTS-3808 § 2.1's strip boundary, so a boundary past the head line makes the
// migration cut INTO the body: 2,331 characters on one bullet of that project,
// which has not migrated yet. INV-5 ("no bullet text is lost across
// migrate-then-render") was reachable here all along; this project's own
// roadmap writes `[ID] **headline**` and never enters the branch, which is why
// 2,052 local bullets showed nothing.
TEST(roadmap_query_emoji_bold_id, Ants4417NextBoldStaysOnTheHeadLine) {
    // The trailer keys are what a real bullet's body always carries, and they
    // are bold — so the body-wide search had something to find on every item.
    const QString withBody = QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B **LOTTO-0011** Stop saying \"still claimable\".\n"
        "  The bank pays automatically, so the wording is wrong.\n"
        "  **Layman:** the page claims money is owed when it already "
        "arrived.\n"
        "  Kind: fix.\n");
    const auto bullets = RoadmapDialog::parseBullets(withBody);
    ASSERT_EQ(bullets.size(), 1);
    const auto &b = bullets[0];

    EXPECT_EQ(b.id, QStringLiteral("LOTTO-0011"));

    // Asserted as EQUALITY, deliberately. ANTS-4378's `EXPECT_FALSE(
    // startsWith(id))` is satisfied by a body fragment just as well as by the
    // real headline, so it could not separate this defect from a pass.
    EXPECT_EQ(b.headline,
              QStringLiteral("Stop saying \"still claimable\"."))
        << "the headline must be the prose on the ID's OWN line; a body "
           "fragment here is the ANTS-4417 defect. got: "
        << b.headline.toStdString();

    // The strip boundary must not reach past the head line.
    const int headLineLen = int(b.body.indexOf(QLatin1Char('\n')));
    ASSERT_GT(headLineLen, 0) << "fixture must have a continuation body";
    EXPECT_LE(b.headlineEnd, headLineLen)
        << "ANTS-3808 § 2.1's strip boundary escaped the head line, so "
           "migration would delete body text. headlineEnd="
        << b.headlineEnd << " headLineLen=" << headLineLen;
}

// ANTS-4417 control — the DOTTED two-bold form `**Sh4.** **Headline.**`, whose
// real headline IS the second bold run on the head line, must keep working.
// This is the shape the next-bold search was written for, and the case a
// head-line clamp would break if it tested the match's END instead of its
// START.
TEST(roadmap_query_emoji_bold_id, Ants4417SecondBoldOnHeadLineStillAdopted) {
    const QString twoBold = QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B **Sh4.** **The real headline.**\n"
        "  **Layman:** body bold that must not be adopted.\n");
    const auto bullets = RoadmapDialog::parseBullets(twoBold);
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_EQ(bullets[0].id, QStringLiteral("Sh4"));
    EXPECT_EQ(bullets[0].headline, QStringLiteral("The real headline."))
        << "a second bold run ON THE HEAD LINE is still the headline; got: "
        << bullets[0].headline.toStdString();
}

// ANTS-4417 control — ANTS-1561's soft-wrapped bold headline. The bold span
// OPENS on the head line and CLOSES on the next, so its match END is past the
// head line while its START is not. Testing the start is what keeps this
// working; testing the end would truncate every soft-wrapped headline in the
// corpus. Five projects on this machine carry this shape (verified 2026-08-17
// by a verdict diff: 0 rows moved in any of them).
TEST(roadmap_query_emoji_bold_id, Ants4417SoftWrappedBoldHeadlineSurvives) {
    const QString wrapped = QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [MC-1065] **Test-audit dedup nits beyond\n"
        "  [MC-1054].**\n"
        "  Kind: test.\n");
    const auto bullets = RoadmapDialog::parseBullets(wrapped);
    ASSERT_EQ(bullets.size(), 1);
    EXPECT_TRUE(bullets[0].headline.contains(QStringLiteral("MC-1054")))
        << "a soft-wrapped bold headline must be captured whole across the "
           "line break (ANTS-1561); got: "
        << bullets[0].headline.toStdString();
}
