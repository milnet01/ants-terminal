// Feature-conformance test for tests/features/
// roadmap_current_preset_excludes_shipped/spec.md.
//
// Locks the ANTS-1423 fix: the Current preset must not let ✅
// shipped bullets through via the current-signal rescue. Drives the
// pure-static `renderCardsHtml` + `renderHtml` helpers against a
// synthetic markdown fixture and a hand-crafted currentBullets list
// that targets specific bullet bodies.
//
// Repro note: bulletPayload truncates the body at the first `**` so
// for ROADMAP-format bullets `- ✅ [ANTS-NNNN] **headline** body`
// the payload reduces to "[ANTS-NNNN] " — meaning the current-signal
// fuzzy compares "ants NNNN" against each signal. To make a signal
// match a specific bullet, embed the bullet's ANTS-NNNN in the
// signal string.

#include "../../_support/expect.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QString>
#include <QStringList>

ANTS_TEST_SCOPE();

namespace {

// Fixture with one ✅ + one 🚧 + one 📋. Each bullet's ANTS-NNNN
// is embedded in a tailored currentBullets entry per test.
QString fixtureMarkdown() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.7.92 — in flight\n"
        "\n"
        "### Bundle\n"
        "\n"
        "- ✅ [ANTS-1100] **Shipped widget alpha bravo charlie.** Body.\n"
        "  Kind: implement.\n"
        "\n"
        "- 🚧 [ANTS-1200] **In-progress delta echo foxtrot.** Body.\n"
        "  Kind: refactor.\n"
        "\n"
        "- 📋 [ANTS-1300] **Planned golf hotel india juliet.** Body.\n"
        "  Kind: fix.\n");
}

bool contains(const QString &hay, const char *needle) {
    return hay.contains(QString::fromUtf8(needle));
}

}  // namespace

using RD = RoadmapDialog;

// INV-1 — renderCardsHtml: Current preset drops ✅ even when the
// current-signal matches its body via the ANTS-NNNN fuzzy hit.
TEST(roadmap_current_preset_excludes_shipped,
     Inv1CardsCurrentPresetDropsShipped) {
    expect_reset();
    QStringList currentBullets;
    currentBullets << QStringLiteral(
        "ANTS-1100 — newly shipped widget, Bundle C closeout.");

    RD::CardRenderOptions opts;
    opts.activePreset = RD::Preset::Current;
    opts.expandedSections.insert(QStringLiteral("bundle"));

    const unsigned filter = RD::filterFor(RD::Preset::Current);
    const QString html = RD::renderCardsHtml(
        fixtureMarkdown(), filter, currentBullets,
        QStringLiteral("light"), RD::SortOrder::Document,
        QString(), {}, opts);

    expect(!contains(html, "ANTS-1100"),
           "INV-1: ✅ ANTS-1100 must NOT appear under Current preset "
           "even when currentBullets mentions its ANTS-NNNN");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — renderCardsHtml: Full preset still surfaces ✅ via the
// status filter (wantDone=true). Gate is a no-op.
TEST(roadmap_current_preset_excludes_shipped,
     Inv2CardsFullPresetKeepsShipped) {
    expect_reset();
    QStringList currentBullets;
    currentBullets << QStringLiteral(
        "ANTS-1100 — newly shipped widget, Bundle C closeout.");

    RD::CardRenderOptions opts;
    opts.activePreset = RD::Preset::Full;
    opts.expandedSections.insert(QStringLiteral("bundle"));

    const unsigned filter = RD::filterFor(RD::Preset::Full);
    const QString html = RD::renderCardsHtml(
        fixtureMarkdown(), filter, currentBullets,
        QStringLiteral("light"), RD::SortOrder::Document,
        QString(), {}, opts);

    expect(contains(html, "ANTS-1100"),
           "INV-2: ✅ ANTS-1100 MUST appear under Full preset");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — renderHtml (v1 path used by roadmap-query IPC): same fix.
TEST(roadmap_current_preset_excludes_shipped,
     Inv3RenderHtmlCurrentPresetDropsShipped) {
    expect_reset();
    QStringList currentBullets;
    currentBullets << QStringLiteral(
        "ANTS-1100 — newly shipped widget, Bundle C closeout.");

    const unsigned filter = RD::filterFor(RD::Preset::Current);
    const QString html = RD::renderHtml(
        fixtureMarkdown(), filter, currentBullets,
        QStringLiteral("light"));

    expect(!contains(html, "ANTS-1100"),
           "INV-3: ✅ ANTS-1100 must NOT appear under Current preset "
           "in renderHtml either");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — Current preset still lets 📋 through via the
// current-signal rescue (the fix narrows the gate, doesn't disable
// it). Targets the 📋 bullet with its ANTS-NNNN in the signal.
TEST(roadmap_current_preset_excludes_shipped,
     Inv4CardsCurrentPresetKeepsPlannedViaSignal) {
    expect_reset();
    QStringList currentBullets;
    currentBullets << QStringLiteral(
        "ANTS-1300 — newly planned bundle work, just queued.");

    RD::CardRenderOptions opts;
    opts.activePreset = RD::Preset::Current;
    opts.expandedSections.insert(QStringLiteral("bundle"));

    const unsigned filter = RD::filterFor(RD::Preset::Current);
    const QString html = RD::renderCardsHtml(
        fixtureMarkdown(), filter, currentBullets,
        QStringLiteral("light"), RD::SortOrder::Document,
        QString(), {}, opts);

    expect(contains(html, "ANTS-1300"),
           "INV-4: 📋 ANTS-1300 MUST still appear under Current "
           "preset via current-signal rescue (Current = "
           "ShowInProgress | ShowCurrent, so 📋 only enters via "
           "the signal)");
    EXPECT_EQ(0, expect_failures());
}
