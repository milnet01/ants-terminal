// ANTS-5645 — the side tab list panel. Contract: spec.md beside this file.
// INV-1..5 drive the widget offscreen; INV-6 round-trips Config in a temp
// XDG_CONFIG_HOME; INV-7 and the setTabLabel half of INV-1 read the
// MainWindow sources (a MainWindow cannot be built headless here).

#include "tablistpanel.h"
#include "config.h"

#include "../../_support/xdg_guard.h"
#include "../../_support/srcgrep.h"

#include <QTemporaryDir>
#include <QTreeWidget>
#include <QWidget>

#include <gtest/gtest.h>

namespace {

using Glyph = ClaudeTabIndicator::Glyph;

// The first two palette entries, so the expected header names come from
// the same table the panel reads.
QColor paletteColor(int i) { return ColoredTabBar::palette().at(i).color; }
QString paletteName(int i) { return ColoredTabBar::palette().at(i).name; }

struct Pages {
    QWidget a, b, c, d;
};

// Every tab row in display order: (title, page).
QVector<QTreeWidgetItem *> tabRows(QTreeWidget *tree) {
    QVector<QTreeWidgetItem *> rows;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree->topLevelItem(i);
        if (top->childCount() == 0 && top->flags() & Qt::ItemIsSelectable) {
            rows << top;
            continue;
        }
        for (int j = 0; j < top->childCount(); ++j)
            rows << top->child(j);
    }
    return rows;
}

}  // namespace

TEST(TabListPanel, Inv1OneRowPerTabWithTheWholeTitle) {
    Pages p;
    const QString longTitle =
        QStringLiteral("A project name far longer than the thirty-character tab cut");
    TabListPanel panel;
    panel.setEntries({{&p.a, longTitle, QColor(), Glyph::None},
                      {&p.b, QStringLiteral("Second"), QColor(), Glyph::None}},
                     &p.a);
    const auto rows = tabRows(panel.treeForTest());
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0)->text(0), longTitle);
    EXPECT_EQ(rows.at(1)->text(0), QStringLiteral("Second"));
}

TEST(TabListPanel, Inv1MainWindowCutsLabelsOnlyInSetTabLabel) {
    const std::string src = ants_test::slurpMainWindow();
    ASSERT_FALSE(src.empty());
    const std::string body = ants_test::slurpFunctionBody(src, "void MainWindow::setTabLabel(");
    ASSERT_FALSE(body.empty()) << "MainWindow::setTabLabel is missing";
    EXPECT_NE(body.find("left(27)"), std::string::npos);
    EXPECT_NE(body.find("m_tabFullTitles[w] = full;"), std::string::npos)
        << "setTabLabel must keep the whole title, not the cut one";
    // Every other cut would hand the panel a shortened title.
    std::size_t count = 0;
    for (std::size_t at = src.find("left(27)"); at != std::string::npos;
         at = src.find("left(27)", at + 1))
        ++count;
    EXPECT_EQ(count, 1u) << "a tab label is cut outside setTabLabel";
}

TEST(TabListPanel, Inv2GroupsByColourInFirstTabOrderUncolouredLast) {
    Pages p;
    TabListPanel panel;
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::None},
                      {&p.b, QStringLiteral("b"), paletteColor(1), Glyph::None},
                      {&p.c, QStringLiteral("c"), paletteColor(0), Glyph::None},
                      {&p.d, QStringLiteral("d"), paletteColor(1), Glyph::None}},
                     &p.a);
    QTreeWidget *tree = panel.treeForTest();
    ASSERT_EQ(tree->topLevelItemCount(), 3);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), paletteName(1));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), paletteName(0));
    EXPECT_EQ(tree->topLevelItem(2)->text(0), QStringLiteral("No colour"));
    ASSERT_EQ(tree->topLevelItem(0)->childCount(), 2);
    EXPECT_EQ(tree->topLevelItem(0)->child(0)->text(0), QStringLiteral("b"));
    EXPECT_EQ(tree->topLevelItem(0)->child(1)->text(0), QStringLiteral("d"));
    EXPECT_EQ(tree->topLevelItem(2)->child(0)->text(0), QStringLiteral("a"));
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        EXPECT_FALSE(tree->topLevelItem(i)->flags() & Qt::ItemIsSelectable);
}

TEST(TabListPanel, Inv2NoColourAnywhereMeansNoHeaders) {
    Pages p;
    TabListPanel panel;
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::None},
                      {&p.b, QStringLiteral("b"), QColor(), Glyph::None}},
                     &p.a);
    QTreeWidget *tree = panel.treeForTest();
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    EXPECT_EQ(tree->topLevelItem(0)->childCount(), 0);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("a"));
}

TEST(TabListPanel, Inv2HeaderNames) {
    EXPECT_EQ(TabListPanel::groupName(paletteColor(0)), paletteName(0));
    EXPECT_EQ(TabListPanel::groupName(QColor(1, 2, 3)), QStringLiteral("Custom colour"));
    EXPECT_EQ(TabListPanel::groupName(QColor()), QStringLiteral("No colour"));
}

TEST(TabListPanel, Inv3ClickSwitchesAndCurrentIsSelected) {
    Pages p;
    TabListPanel panel;
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::None},
                      {&p.b, QStringLiteral("b"), QColor(), Glyph::None}},
                     &p.b);
    QTreeWidget *tree = panel.treeForTest();
    const auto rows = tabRows(tree);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(tree->currentItem(), rows.at(1));

    QVector<QWidget *> activated;
    QObject::connect(&panel, &TabListPanel::tabActivated,
                     [&activated](QWidget *page) { activated << page; });
    emit tree->itemClicked(rows.at(0), 0);
    ASSERT_EQ(activated.size(), 1);
    EXPECT_EQ(activated.at(0), &p.a);
    emit tree->itemActivated(rows.at(1), 0);
    ASSERT_EQ(activated.size(), 2);
    EXPECT_EQ(activated.at(1), &p.b);
}

TEST(TabListPanel, Inv4StateIsNamedInWords) {
    Pages p;
    TabListPanel panel;
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::AwaitingInput},
                      {&p.b, QStringLiteral("b"), QColor(), Glyph::None}},
                     &p.a);
    const auto rows = tabRows(panel.treeForTest());
    ASSERT_EQ(rows.size(), 2);
    const QString label = TabListPanel::stateLabel(Glyph::AwaitingInput);
    ASSERT_FALSE(label.isEmpty());
    EXPECT_TRUE(rows.at(0)->toolTip(0).contains(label));
    EXPECT_TRUE(rows.at(0)->data(0, Qt::AccessibleTextRole).toString().contains(label));
    EXPECT_TRUE(TabListPanel::stateLabel(Glyph::None).isEmpty());
    EXPECT_FALSE(rows.at(1)->toolTip(0).contains(QStringLiteral("Claude")));
}

TEST(TabListPanel, Inv5FontFollowsTheGivenSize) {
    Pages p;
    TabListPanel panel;
    panel.setFontPointSize(22);
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::None}}, &p.a);
    EXPECT_EQ(panel.treeForTest()->font().pointSize(), 22);
}

TEST(TabListPanel, Inv6ConfigKeysRoundTrip) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    guard.setEnv("XDG_CONFIG_HOME", tmp.path().toUtf8());
    {
        Config c;
        EXPECT_FALSE(c.tabListPanel());
        EXPECT_FALSE(c.tabListPanelHidesTabBar());
        EXPECT_EQ(c.tabListPanelWidth(), 280);
        c.setTabListPanel(true);
        c.setTabListPanelHidesTabBar(true);
        c.setTabListPanelWidth(400);
    }
    Config again;
    EXPECT_TRUE(again.tabListPanel());
    EXPECT_TRUE(again.tabListPanelHidesTabBar());
    EXPECT_EQ(again.tabListPanelWidth(), 400);
}

TEST(TabListPanel, Inv7BothReloadPathsApplyThePanel) {
    const std::string src = ants_test::slurpMainWindow();
    ASSERT_FALSE(src.empty());
    const std::string reload =
        ants_test::slurpFunctionBody(src, "void MainWindow::onConfigFileChanged(");
    ASSERT_FALSE(reload.empty());
    EXPECT_NE(reload.find("applyTabListPanelConfig()"), std::string::npos);
    const std::string dialog =
        ants_test::slurpFunctionBody(src, "&SettingsDialog::settingsChanged, this, [this]()");
    ASSERT_FALSE(dialog.empty());
    EXPECT_NE(dialog.find("applyTabListPanelConfig()"), std::string::npos);
}
