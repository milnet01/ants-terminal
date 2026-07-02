// ANTS-1428 Tier 3 — dialog renderer parity (INV-10). Behavioural
// tests against `RoadmapDialog::renderCardsHtml`. Asserts on the
// emitted HTML so an unintentional refactor that drops the GFM
// support trips loud.

#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace {

QString render(const QString &md,
               const QStringList &expandSlugs = {"phase-11a", "section"}) {
    // Default filter mask shows every status category (matches the
    // Full preset). Empty currentBullets so the rm-current branch
    // doesn't decorate anything; default Cozy density. Cards only
    // emit when their section is `expandedSections`; pass the
    // expected slugs so the per-bullet HTML actually appears.
    const unsigned filter =
        RoadmapDialog::ShowDone | RoadmapDialog::ShowPlanned |
        RoadmapDialog::ShowInProgress | RoadmapDialog::ShowConsidered;
    RoadmapDialog::CardRenderOptions opts;
    opts.activePreset = RoadmapDialog::Preset::Full;
    for (const QString &slug : expandSlugs) opts.expandedSections.insert(slug);
    return RoadmapDialog::renderCardsHtml(
        md, filter, /*currentBullets*/ {}, /*themeName*/ "default",
        RoadmapDialog::SortOrder::Document, QString(),
        QSet<QString>(), opts);
}

int countSubstr(const QString &hay, const QString &needle) {
    int n = 0, pos = 0;
    while ((pos = hay.indexOf(needle, pos)) >= 0) {
        ++n;
        pos += needle.size();
    }
    return n;
}

}  // namespace

// INV-10 — one card per GFM bullet.
TEST(AdapterRenderGfm, OneCardPerBullet) {
    const QString md = QStringLiteral(
        "# Vestige\n\n"
        "## Phase 11A\n"
        "- [x] **Sh4.** Shader work goes here\n"
        "- [ ] **Ed1.** Editor work goes here\n"
        "- [ ] Headline without bold-id\n");
    const QString html = render(md);
    // ANTS-3392 — cards are `<tr class="rm-card">` rows. Match the row
    // opening tag, not the bare `class="rm-card"` substring, which now
    // also collides with the section's `<table class="rm-cards">` tag.
    EXPECT_EQ(countSubstr(html, QStringLiteral("<tr class=\"rm-card")), 3)
        << html.toUtf8().constData();
}

// INV-10 — status icon mapping. `- [x]` renders the ✅ glyph;
// `- [ ]` renders 📋.
TEST(AdapterRenderGfm, StatusIconMapping) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [x] Done item\n"
        "- [ ] Planned item\n");
    const QString html = render(md);
    EXPECT_TRUE(html.contains(QStringLiteral("✅")))
        << html.toUtf8().constData();
    EXPECT_TRUE(html.contains(QStringLiteral("📋")));
}

// INV-10 — inline-emoji override (e.g. 🚧 ahead of [x]) wins.
TEST(AdapterRenderGfm, InlineEmojiOverride) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] 🚧 In-progress override\n");
    const QString html = render(md);
    EXPECT_TRUE(html.contains(QStringLiteral("🚧")))
        << html.toUtf8().constData();
}

// INV-10 — synthetic-ID cards carry rm-card-synthetic class on
// the card row (not just the CSS stylesheet rule). ANTS-3392: the
// card container is a `<tr>`, not a `<div>`.
TEST(AdapterRenderGfm, SyntheticCardCarriesClass) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] Headline without bold-id\n");
    const QString html = render(md);
    EXPECT_GE(countSubstr(html, QStringLiteral(
        "<tr class=\"rm-card rm-card-synthetic")), 1)
        << html.toUtf8().constData();
}

// INV-10 — bold-ID cards do NOT carry rm-card-synthetic on the
// card row. ANTS-3392: the container is a `<tr>`; the synthetic
// accent moved to a `.rm-col-syn` first-cell class, but the row
// still carries the `rm-card-synthetic` marker class for synthetic
// bullets only. Assert no decorated row appears, and the bare
// `<tr class="rm-card">` row appears once.
TEST(AdapterRenderGfm, BoldIdCardLacksSyntheticClass) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **Sh4.** Bold-ID bullet\n");
    const QString html = render(md);
    EXPECT_EQ(countSubstr(html, QStringLiteral(
        "<tr class=\"rm-card rm-card-synthetic")), 0)
        << html.toUtf8().constData();
    EXPECT_GE(countSubstr(html, QStringLiteral(
        "<tr class=\"rm-card\"")), 1)
        << html.toUtf8().constData();
}

// INV-10 — the dashed-border CSS rule for synthetic cards is emitted
// in the stylesheet. ANTS-3392 moved the accent to the first cell:
// `.rm-col-syn{border-left-style:dashed;}`.
TEST(AdapterRenderGfm, SyntheticCssRulePresent) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] Some bullet\n");
    const QString html = render(md);
    EXPECT_TRUE(html.contains(QStringLiteral(".rm-col-syn")))
        << html.toUtf8().constData();
    EXPECT_TRUE(html.contains(QStringLiteral("border-left-style:dashed")))
        << html.toUtf8().constData();
}

// INV-10 — kind/lanes/layman chips suppressed when source bullet
// lacks those fields. The rm-kind span is only emitted when
// rec.kind is non-empty; GFM bullets don't have Kind: lines so
// no rm-kind span should appear.
TEST(AdapterRenderGfm, KindChipSuppressedWhenEmpty) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **Sh4.** Bullet with no Kind line\n");
    const QString html = render(md);
    EXPECT_FALSE(html.contains(QStringLiteral("class=\"rm-kind\"")))
        << html.toUtf8().constData();
}

// INV-10 — bold-ID rendered verbatim in the ID slot.
TEST(AdapterRenderGfm, BoldIdRenderedInIdSlot) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] **VEST-0042.** Cross-cutting fix\n");
    const QString html = render(md);
    // The renderer prefixes non-ANTS- IDs with '#' (see
    // hashedId in renderCardsHtml).
    EXPECT_TRUE(html.contains(QStringLiteral("#VEST-0042")))
        << html.toUtf8().constData();
}

// INV-10 — synthetic ID rendered as the raw 10-char hash (parser
// produces it without project prefix; dialog renders #hash).
TEST(AdapterRenderGfm, SyntheticIdRenderedAsHash) {
    const QString md = QStringLiteral(
        "## Section\n"
        "- [ ] Headline without bold-id\n");
    const QString html = render(md);
    // Find the rm-id span content. The id is 10 base36 chars; we
    // just check for the '#' prefix on a 10-char base36 word.
    static const QRegularExpression rx(
        QStringLiteral("#[0-9a-z]{10}"));
    EXPECT_TRUE(rx.match(html).hasMatch())
        << html.toUtf8().constData();
}
