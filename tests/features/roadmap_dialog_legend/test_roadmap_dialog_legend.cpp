// Feature-conformance test for tests/features/roadmap_dialog_legend/spec.md.
//
// Locks ANTS-3793 § 2.3 — a store-served project renders its OWN legend, a
// store-served project with none renders none, and every markdown-served
// project keeps the dialog's compile-time labels. The third row is the
// regression guard: it is the one a careless implementation of the first two
// removes, and "most projects" is what it protects during the rollout.
//
// Behavioural against a real on-disk store plus the static renderer. Exit 0 =
// all invariants hold.

#include "roadmapdialog.h"
#include "roadmapsource.h"
#include "roadmapstore.h"

#include <QDir>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {

using RD = RoadmapDialog;

constexpr unsigned kAllOn = RD::ShowDone | RD::ShowPlanned
                          | RD::ShowInProgress | RD::ShowConsidered;

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// One ✅ bullet under one section, which is all three rows need: the label
// span is emitted per card, and the status is what the legend is keyed by.
QString fixtureMarkdown() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.8.0 — feature delivery\n"
        "\n"
        "### Features\n"
        "\n"
        "- ✅ [ANTS-9001] **A shipped thing.** Body text.\n"
        "  Kind: implement.\n");
}

QString renderWith(const QHash<QString, QString> &legend, bool fromStore) {
    RD::CardRenderOptions opts;
    opts.activePreset = RD::Preset::Full;
    opts.expandedSections.insert(QStringLiteral("features"));
    opts.legend = legend;
    opts.legendFromStore = fromStore;
    return RD::renderCardsHtml(fixtureMarkdown(), kAllOn, {},
                               QStringLiteral("light"),
                               RD::SortOrder::Document, QString(), {}, opts);
}

// NEVER default-construct RoadmapStore: it resolves defaultPath() under
// XDG_DATA_HOME — the developer's REAL store — and every case here would write
// into it. Access is the THIRD parameter, after the history cap.
std::unique_ptr<RoadmapStore> openStore(const QTemporaryDir &dir) {
    auto store = std::make_unique<RoadmapStore>(
        dir.filePath(QStringLiteral("store.db")),
        RoadmapStore::kDefaultHistoryCapBytes,
        RoadmapStore::Access::Interactive);
    QString err;
    EXPECT_TRUE(store->open(&err)) << err.toStdString();
    return store;
}

}  // namespace

// § 2.3's table, row by row, plus the re-keying the first row depends on.
TEST(RoadmapDialogLegend, Inv2Legend) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto store = openStore(dir);
    ASSERT_TRUE(store->isOpen());

    const QString withLegendRoot = dir.filePath(QStringLiteral("with"));
    const QString noLegendRoot   = dir.filePath(QStringLiteral("without"));
    ASSERT_TRUE(QDir().mkpath(withLegendRoot));
    ASSERT_TRUE(QDir().mkpath(noLegendRoot));

    QString err;
    const auto withPid = store->registerProject(
        withLegendRoot, QStringLiteral("With"), QStringLiteral("with"), &err);
    ASSERT_TRUE(withPid.has_value()) << err.toStdString();
    const auto noPid = store->registerProject(
        noLegendRoot, QStringLiteral("Without"), QStringLiteral("without"), &err);
    ASSERT_TRUE(noPid.has_value()) << err.toStdString();

    QJsonObject legend;
    legend.insert(QStringLiteral("shipped"),  QStringLiteral("delivered"));
    legend.insert(QStringLiteral("planned"),  QStringLiteral("queued"));
    ASSERT_TRUE(store->setLegend(*withPid, legend, &err)) << err.toStdString();

    // --- the re-keying (store lifecycle WORDS → the status EMOJI every
    // BulletRecord consumer is keyed by). Without it row 1 cannot hold.
    const auto withRow = store->readProject(*withPid, &err);
    ASSERT_TRUE(withRow.has_value()) << err.toStdString();
    ASSERT_FALSE(withRow->legendText.isEmpty());
    const QHash<QString, QString> byEmoji =
        RoadmapSource::legendByEmoji(withRow->legendText);
    EXPECT_EQ(byEmoji.size(), 2);
    EXPECT_EQ(byEmoji.value(QString::fromUtf8("\xE2\x9C\x85")),
              QStringLiteral("delivered"));
    EXPECT_EQ(byEmoji.value(QString::fromUtf8("\xF0\x9F\x93\x8B")),
              QStringLiteral("queued"));

    // A project that stored no legend has none to render — not an empty
    // string, not a default.
    const auto noRow = store->readProject(*noPid, &err);
    ASSERT_TRUE(noRow.has_value()) << err.toStdString();
    EXPECT_TRUE(RoadmapSource::legendByEmoji(noRow->legendText).isEmpty());

    // INV-4 — a lifecycle word with no glyph is skipped, never inserted under
    // an empty key. roadmap-format.md § 3.11 gives `dropped` no emoji.
    const QHash<QString, QString> dropped = RoadmapSource::legendByEmoji(
        QStringLiteral("{\"dropped\":\"gone\",\"shipped\":\"out\"}"));
    EXPECT_EQ(dropped.size(), 1);
    EXPECT_FALSE(dropped.contains(QString()));

    // --- row 1: store-served, with a stored legend → that legend's label.
    const std::string row1 = renderWith(byEmoji, /*fromStore=*/true).toStdString();
    EXPECT_TRUE(contains(row1,
        "<span class=\"rm-state-label\">delivered</span>"))
        << "row 1: the project's stored label is not rendered";
    EXPECT_FALSE(contains(row1, ">shipped</span>"))
        << "row 1: the compile-time label leaked onto the store path";

    // --- row 2: store-served, no stored legend → no label span at all.
    // The bare class name appears in the stylesheet, so the assertion is on
    // the emitted SPAN — `<span class="rm-state-label">` — not on the token.
    const std::string row2 =
        renderWith(QHash<QString, QString>(), /*fromStore=*/true).toStdString();
    EXPECT_FALSE(contains(row2, "<span class=\"rm-state-label\">"))
        << "row 2: a project with no stored legend rendered one anyway";

    // --- row 3: markdown-served → today's compile-time labels, unchanged.
    // This is the regression guard, and the reason § 2.3 is a branch rather
    // than a replacement.
    const std::string row3 =
        renderWith(QHash<QString, QString>(), /*fromStore=*/false).toStdString();
    EXPECT_TRUE(contains(row3,
        "<span class=\"rm-state-label\">shipped</span>"))
        << "row 3: an unmigrated project lost the dialog's built-in legend";
}
