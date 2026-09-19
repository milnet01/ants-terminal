// ANTS-4977 INV-9 — the roadmap dialog filters 🚫 like any other status.
// Contract: tests/features/roadmap_dropped_status/spec.md
//
// The dialogs half of the feature test: RoadmapDialog links only here.

#include "config.h"
#include "roadmapdialog.h"

#include "../../_support/xdg_guard.h"

#include <QCheckBox>
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <string>

namespace {

using RD = RoadmapDialog;

QString fixture() {
    return QString::fromUtf8(
        "# Sample Roadmap\n"
        "\n"
        "## Work\n"
        "\n"
        "- \xE2\x9C\x85 [ANTS-9001] **A shipped thing.**\n"
        "  Kind: implement.\n"
        "- \xF0\x9F\x9A\xAB [ANTS-9002] **A dropped thing.**\n"
        "  Kind: implement.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9003] **A planned thing.**\n"
        "  Kind: implement.\n");
}

std::string cards(unsigned filter) {
    RD::CardRenderOptions opts;
    opts.activePreset = RD::Preset::Custom;
    opts.expandedSections.insert(QStringLiteral("work"));
    return RD::renderCardsHtml(fixture(), filter, {}, QStringLiteral("light"),
                               RD::SortOrder::Document, QString(), {}, opts)
        .toStdString();
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(RoadmapDroppedStatusDialog, Inv9DroppedObeysTheFilter) {
    const unsigned allButDropped = RD::ShowDone | RD::ShowPlanned
                                 | RD::ShowInProgress | RD::ShowConsidered;
    const std::string off = cards(allButDropped);
    EXPECT_TRUE(has(off, "ANTS-9001"));
    EXPECT_FALSE(has(off, "ANTS-9002"))
        << "a 🚫 card rendered with Dropped unticked";

    const std::string on = cards(allButDropped | RD::ShowDropped);
    EXPECT_TRUE(has(on, "ANTS-9002")) << "a 🚫 card is missing with Dropped ticked";
}

TEST(RoadmapDroppedStatusDialog, Inv9HistoryIsDoneAndDropped) {
    EXPECT_EQ(RD::filterFor(RD::Preset::History), unsigned(RD::ShowDone | RD::ShowDropped));
    EXPECT_NE(RD::filterFor(RD::Preset::Full) & RD::ShowDropped, 0u);
    for (RD::Preset p : {RD::Preset::Current, RD::Preset::Next, RD::Preset::FarFuture})
        EXPECT_EQ(RD::filterFor(p) & RD::ShowDropped, 0u);
}

// A filter saved before 🚫 existed has no `dropped` key; it must not hide
// dropped items.
TEST(RoadmapDroppedStatusDialog, Inv9AbsentConfigKeyReadsAsTicked) {
    ants_test::XdgGuard guard;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    guard.setTestMode(false);   // see roadmap_filter_bar's Harness
    guard.setEnv("XDG_CONFIG_HOME", dir.path().toUtf8());
    const QString path = dir.filePath(QStringLiteral("ROADMAP.md"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(fixture().toUtf8());
    }
    Config cfg;
    cfg.setRoadmapActivePreset(QStringLiteral("custom"));
    QJsonObject sf;
    sf[QStringLiteral("done")]        = true;
    sf[QStringLiteral("planned")]     = false;   // proves the saved set was read
    sf[QStringLiteral("in_progress")] = true;
    sf[QStringLiteral("considered")]  = true;
    sf[QStringLiteral("current")]     = true;
    cfg.setRoadmapStatusFilters(sf);

    RoadmapDialog dlg(path, QStringLiteral("light"), nullptr, &cfg);
    auto *planned = dlg.findChild<QCheckBox *>(QStringLiteral("roadmap-filter-planned"));
    auto *dropped = dlg.findChild<QCheckBox *>(QStringLiteral("roadmap-filter-dropped"));
    ASSERT_NE(planned, nullptr);
    ASSERT_NE(dropped, nullptr) << "no Dropped checkbox";
    EXPECT_FALSE(planned->isChecked()) << "the saved custom filter was not restored";
    EXPECT_TRUE(dropped->isChecked()) << "an absent `dropped` key hid dropped items";
}
