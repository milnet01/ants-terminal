// ANTS-5645 — the side tab list panel. Contract: spec.md beside this file.
// INV-1..5 drive the widget offscreen; INV-6 round-trips Config in a temp
// XDG_CONFIG_HOME; INV-7 and the setTabLabel half of INV-1 read the
// MainWindow sources (a MainWindow cannot be built headless here).

#include "tablistpanel.h"
#include "config.h"

#include "../../_support/xdg_guard.h"
#include "../../_support/srcgrep.h"

#include <QAbstractItemDelegate>
#include <QImage>
#include <QLineEdit>
#include <QPainter>
#include <QStyleOptionViewItem>
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

// ANTS-5659 — a new tab brought the hidden tab bar back: four sites set
// its visibility from the tab count alone. One function now decides.
TEST(TabListPanel, Inv11OneFunctionDecidesTheTabBar) {
    const std::string src = ants_test::stripComments(ants_test::slurpMainWindow());
    ASSERT_FALSE(src.empty());
    const auto count = [&](const std::string &needle) {
        int n = 0;
        for (size_t at = src.find(needle); at != std::string::npos; at = src.find(needle, at + 1))
            ++n;
        return n;
    };
    EXPECT_EQ(count("tabBar()->setVisible(") + count("m_coloredTabBar->setVisible("), 1)
        << "the tab bar's visibility is set outside updateTabBarVisibility()";
    const std::string body =
        ants_test::slurpFunctionBody(src, "void MainWindow::updateTabBarVisibility(");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("setVisible("), std::string::npos);
    EXPECT_NE(body.find("count() > 1"), std::string::npos) << "one tab needs no bar";
    EXPECT_NE(body.find("tabListPanelHidesTabBar()"), std::string::npos)
        << "the side list's hide setting is ignored";
    EXPECT_GE(count("updateTabBarVisibility()"), 6) << "a tab-count site does not call it";
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

// ---------------------------------------------------------------- INV-8 -----
// ANTS-5650 — the row's background is its Claude state colour, and the name
// stays readable under hover and selection.

namespace {

const QColor kDarkBg(0x1e, 0x1e, 0x2e);
const QColor kDarkText(0xcd, 0xd6, 0xf4);
const QColor kLightBg(0xef, 0xf1, 0xf5);

const Glyph kAllGlyphs[] = {Glyph::None,     Glyph::Idle,      Glyph::Thinking,
                            Glyph::ToolUse,  Glyph::Bash,      Glyph::Planning,
                            Glyph::Auditing, Glyph::Compacting, Glyph::AwaitingInput};

// Paints one row through the tree's own delegate, as the view would.
QImage paintRow(TabListPanel &panel, QTreeWidgetItem *row, QStyle::State extra) {
    QTreeWidget *tree = panel.treeForTest();
    const QModelIndex index = tree->indexFromItem(row);
    QStyleOptionViewItem opt;
    opt.initFrom(tree->viewport());
    opt.widget = tree;
    opt.rect = QRect(0, 0, 400, 32);
    opt.state |= QStyle::State_Enabled | QStyle::State_Active | extra;
    QImage img(opt.rect.size(), QImage::Format_ARGB32);
    img.fill(Qt::magenta);
    QPainter p(&img);
    tree->itemDelegateForIndex(index)->paint(&p, opt, index);
    return img;
}

}  // namespace

TEST(TabListPanel, Inv8FillIsTheStateColourOrThePanelBackground) {
    for (const QColor &bg : {kDarkBg, kLightBg}) {
        EXPECT_EQ(TabListPanel::rowPaint(Glyph::None, bg, false).fill, bg);
        for (Glyph g : kAllGlyphs) {
            if (g == Glyph::None) continue;
            EXPECT_EQ(TabListPanel::rowPaint(g, bg, false).fill,
                      ClaudeTabIndicator::color(g))
                << TabListPanel::stateLabel(g).toStdString();
        }
    }
}

TEST(TabListPanel, Inv8NameReadableOnEveryFillHoveredOrNot) {
    for (const QColor &bg : {kDarkBg, kLightBg}) {
        for (Glyph g : kAllGlyphs) {
            const auto rest = TabListPanel::rowPaint(g, bg, false);
            const auto hover = TabListPanel::rowPaint(g, bg, true);
            const std::string what = TabListPanel::stateLabel(g).toStdString();
            EXPECT_GE(ClaudeTabIndicator::contrastRatio(rest.ink, rest.fill), 4.5) << what;
            EXPECT_GE(ClaudeTabIndicator::contrastRatio(hover.ink, hover.fill), 4.5) << what;
            EXPECT_NE(hover.fill, rest.fill) << "hover must show: " << what;
        }
    }
}

// The reported bug: the app style sheet's item rules gave a hovered,
// selected row its background colour for both fill and text. These are
// those two rules, from src/themedstylesheet.cpp, with the theme filled in.
TEST(TabListPanel, Inv8HoveredCurrentRowStillShowsItsName) {
    Pages p;
    TabListPanel panel;
    panel.setThemeColors(kDarkBg, kDarkText);
    panel.setStyleSheet(QStringLiteral(
        "QTreeWidget { background-color: %1; color: %2;"
        "  selection-background-color: #89b4fa; selection-color: %1; }"
        "QTreeWidget::item:hover { background: %1; }")
                            .arg(kDarkBg.name(), kDarkText.name()));
    const QString title = QStringLiteral("A long tab name that runs well past the stripe");
    panel.setEntries({{&p.a, title, paletteColor(0), Glyph::Thinking}}, &p.a);
    const auto rows = tabRows(panel.treeForTest());
    ASSERT_EQ(rows.size(), 1);

    const QImage img = paintRow(panel, rows.at(0),
                                QStyle::State_Selected | QStyle::State_MouseOver);
    const auto want = TabListPanel::rowPaint(Glyph::Thinking, kDarkBg, true);
    // Measured against the row's own painted background, past the text.
    const QColor painted(img.pixel(380, img.height() / 2));
    int inkish = 0;
    for (int x = 80; x < 300; ++x)
        for (int y = 0; y < img.height(); ++y)
            if (ClaudeTabIndicator::contrastRatio(QColor(img.pixel(x, y)), painted) > 3.0)
                ++inkish;
    EXPECT_GT(inkish, 20) << "the name is not visible on a hovered current row";
    EXPECT_EQ(QColor(img.pixel(380, img.height() / 2)), want.fill)
        << "the row's background is not its hovered state colour";
    EXPECT_EQ(QColor(img.pixel(1, img.height() / 2)).rgb(), paletteColor(0).rgb())
        << "the tab colour stripe is missing";
}

// ANTS-5658 — at rest a state row fades from its state colour, under the
// name, into the panel background; hovered, it is one colour.
TEST(TabListPanel, Inv8RestingRowFadesIntoTheBackground) {
    for (const QColor &bg : {kDarkBg, kLightBg}) {
        for (Glyph g : kAllGlyphs) {
            const auto rest = TabListPanel::rowPaint(g, bg, false);
            const auto hover = TabListPanel::rowPaint(g, bg, true);
            const std::string what = TabListPanel::stateLabel(g).toStdString();
            EXPECT_EQ(rest.fade, bg) << what;
            EXPECT_EQ(hover.fade, hover.fill) << "hover must be one colour: " << what;
        }
    }
}

TEST(TabListPanel, Inv8PaintedRowIsAGradientWithAWiderStripe) {
    Pages p;
    TabListPanel panel;
    panel.setThemeColors(kDarkBg, kDarkText);
    const QString name = QStringLiteral("Ants Terminal");
    panel.setEntries({{&p.a, name, paletteColor(0), Glyph::Thinking},
                      {&p.b, QStringLiteral("y"), paletteColor(0), Glyph::None}},
                     &p.b);
    const auto rows = tabRows(panel.treeForTest());
    ASSERT_EQ(rows.size(), 2);
    const auto want = TabListPanel::rowPaint(Glyph::Thinking, kDarkBg, false);

    const QImage rest = paintRow(panel, rows.at(0), {});
    const int mid = rest.height() / 2;
    // 32px row: the ANTS-5650 stripe was max(5, 32/4) = 8px; now 11px.
    EXPECT_EQ(QColor(rest.pixel(10, mid)).rgb(), paletteColor(0).rgb())
        << "the stripe is not 3px wider";
    EXPECT_NE(QColor(rest.pixel(11, mid)).rgb(), paletteColor(0).rgb());
    const auto near = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) <= 8 && qAbs(a.green() - b.green()) <= 8 &&
               qAbs(a.blue() - b.blue()) <= 8;
    };
    EXPECT_TRUE(near(QColor(rest.pixel(12, mid)), want.fill)) << "left end is not the state colour";
    EXPECT_TRUE(near(QColor(rest.pixel(399, mid)), want.fade)) << "right end is not the fade colour";
    EXPECT_FALSE(near(QColor(rest.pixel(12, mid)), QColor(rest.pixel(399, mid))))
        << "the resting row is one flat colour";
    // The name sits on the state colour: no fade until it ends. The stripe
    // is 11px and the pad max(6, 32/4) = 8px, so the name starts at 19.
    const int nameEnd = 19 + QFontMetrics(panel.treeForTest()->viewport()->font())
                                 .horizontalAdvance(name);
    ASSERT_LT(nameEnd, 380);
    EXPECT_EQ(QColor(rest.pixel(nameEnd - 1, 1)), want.fill) << "the fade starts under the name";
    EXPECT_FALSE(near(QColor(rest.pixel((nameEnd + 399) / 2, 1)), want.fill))
        << "nothing fades after the name";

    const QImage hover = paintRow(panel, rows.at(0), QStyle::State_MouseOver);
    const auto hoverWant = TabListPanel::rowPaint(Glyph::Thinking, kDarkBg, true);
    EXPECT_EQ(QColor(hover.pixel(12, mid)), hoverWant.fill);
    EXPECT_EQ(QColor(hover.pixel(399, mid)), hoverWant.fill) << "hover is not a full fill";
}

// ---------------------------------------------------------------- INV-9 -----
// The tab bar's Rename edited over the tab bar, which the panel can hide.

namespace {

QLineEdit *openEditor(TabListPanel &panel, QTreeWidgetItem *row) {
    return qobject_cast<QLineEdit *>(
        panel.treeForTest()->indexWidget(panel.treeForTest()->indexFromItem(row)));
}

void closeEditor(TabListPanel &panel, QLineEdit *editor,
                 QAbstractItemDelegate::EndEditHint hint) {
    QAbstractItemDelegate *d = panel.treeForTest()->itemDelegate();
    if (hint != QAbstractItemDelegate::RevertModelCache) emit d->commitData(editor);
    emit d->closeEditor(editor, hint);
}

}  // namespace

TEST(TabListPanel, Inv9RightClickOnATabRowAsksForTheMenu) {
    Pages p;
    TabListPanel panel;
    panel.resize(300, 300);
    panel.show();
    panel.setEntries({{&p.a, QStringLiteral("a"), paletteColor(0), Glyph::None}}, &p.a);
    QTreeWidget *tree = panel.treeForTest();
    QVector<QWidget *> asked;
    QObject::connect(&panel, &TabListPanel::tabContextMenuRequested,
                     [&asked](QWidget *page) { asked << page; });

    QTreeWidgetItem *header = tree->topLevelItem(0);
    emit tree->customContextMenuRequested(tree->visualItemRect(header).center());
    EXPECT_TRUE(asked.isEmpty()) << "a header asked for a tab menu";

    emit tree->customContextMenuRequested(tree->visualItemRect(tabRows(tree).at(0)).center());
    ASSERT_EQ(asked.size(), 1);
    EXPECT_EQ(asked.at(0), &p.a);
}

TEST(TabListPanel, Inv9RenameEditsTheRowAndSendsAChangedName) {
    Pages p;
    const QString longTitle =
        QStringLiteral("A project name far longer than the thirty-character tab cut");
    TabListPanel panel;
    panel.resize(300, 300);
    panel.show();
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::None},
                      {&p.b, longTitle, QColor(), Glyph::None}},
                     &p.a);
    QVector<QPair<QWidget *, QString>> renamed;
    QObject::connect(&panel, &TabListPanel::tabRenamed,
                     [&renamed](QWidget *page, const QString &n) { renamed.append({page, n}); });

    ASSERT_TRUE(panel.startRename(&p.b));
    QTreeWidgetItem *row = tabRows(panel.treeForTest()).at(1);
    QLineEdit *editor = openEditor(panel, row);
    ASSERT_NE(editor, nullptr) << "no editor opened on the row";
    EXPECT_EQ(editor->text(), longTitle);

    // A refresh while typing must not delete the row under the editor.
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::Thinking},
                      {&p.b, longTitle, QColor(), Glyph::None}},
                     &p.a);
    ASSERT_EQ(tabRows(panel.treeForTest()).at(1), row);
    ASSERT_EQ(openEditor(panel, row), editor);

    editor->setText(QStringLiteral("Build"));
    closeEditor(panel, editor, QAbstractItemDelegate::SubmitModelCache);
    ASSERT_EQ(renamed.size(), 1);
    EXPECT_EQ(renamed.at(0).first, &p.b);
    EXPECT_EQ(renamed.at(0).second, QStringLiteral("Build"));
    // The refresh that arrived during the edit is applied once it closes.
    EXPECT_TRUE(tabRows(panel.treeForTest()).at(0)->toolTip(0).contains(
        QStringLiteral("Claude thinking")));
}

TEST(TabListPanel, Inv9EscapeSendsNothingAndKeepsTheTitle) {
    Pages p;
    TabListPanel panel;
    panel.resize(300, 300);
    panel.show();
    panel.setEntries({{&p.a, QStringLiteral("a"), QColor(), Glyph::None}}, &p.a);
    int renamed = 0;
    QObject::connect(&panel, &TabListPanel::tabRenamed, [&renamed] { ++renamed; });

    ASSERT_TRUE(panel.startRename(&p.a));
    QLineEdit *editor = openEditor(panel, tabRows(panel.treeForTest()).at(0));
    ASSERT_NE(editor, nullptr);
    editor->setText(QStringLiteral("typed then abandoned"));
    closeEditor(panel, editor, QAbstractItemDelegate::RevertModelCache);
    EXPECT_EQ(renamed, 0);
    EXPECT_EQ(tabRows(panel.treeForTest()).at(0)->text(0), QStringLiteral("a"));
    EXPECT_FALSE(panel.startRename(&p.b)) << "a page with no row opened an editor";
}

TEST(TabListPanel, Inv9MainWindowRoutesThePanelMenuAndRename) {
    const std::string src = ants_test::slurpMainWindow();
    ASSERT_FALSE(src.empty());
    const std::string apply =
        ants_test::slurpFunctionBody(src, "void MainWindow::applyTabListPanelConfig(");
    ASSERT_FALSE(apply.empty());
    EXPECT_NE(apply.find("showTabColorMenu(i, true)"), std::string::npos);
    const size_t renamed = apply.find("&TabListPanel::tabRenamed");
    ASSERT_NE(renamed, std::string::npos);
    EXPECT_NE(apply.find("setTabTitleForRemote(", renamed), std::string::npos);
    const std::string menu =
        ants_test::slurpFunctionBody(src, "void MainWindow::showTabColorMenu(");
    ASSERT_FALSE(menu.empty());
    EXPECT_NE(menu.find("startRename("), std::string::npos);
}

// --------------------------------------------------------------- INV-10 -----

namespace {

QStringList rowTitles(QTreeWidget *tree) {
    QStringList out;
    for (QTreeWidgetItem *row : tabRows(tree)) out << row->text(0);
    return out;
}

// Tab order: "tab 10", "Beta", "alpha", "Tab 2"; b and d share a colour.
void setMixedEntries(TabListPanel &panel, Pages &p) {
    panel.setEntries({{&p.a, QStringLiteral("tab 10"), QColor(), Glyph::None},
                      {&p.b, QStringLiteral("Beta"), paletteColor(0), Glyph::None},
                      {&p.c, QStringLiteral("alpha"), QColor(), Glyph::None},
                      {&p.d, QStringLiteral("Tab 2"), paletteColor(0), Glyph::None}},
                     &p.a);
}

}  // namespace

TEST(TabListPanel, Inv10SortKeysParseWithColourAsTheFallback) {
    using S = TabListPanel::SortMode;
    EXPECT_EQ(TabListPanel::sortModeFromKey(QStringLiteral("colour")), S::ByColour);
    EXPECT_EQ(TabListPanel::sortModeFromKey(QStringLiteral("colour_az")), S::ByColourAZ);
    EXPECT_EQ(TabListPanel::sortModeFromKey(QStringLiteral("az")), S::AZ);
    EXPECT_EQ(TabListPanel::sortModeFromKey(QStringLiteral("tab_order")), S::TabOrder);
    EXPECT_EQ(TabListPanel::sortModeFromKey(QStringLiteral("nonsense")), S::ByColour);
    EXPECT_EQ(TabListPanel::sortModeFromKey(QString()), S::ByColour);
    for (S m : {S::ByColour, S::ByColourAZ, S::AZ, S::TabOrder})
        EXPECT_EQ(TabListPanel::sortModeFromKey(TabListPanel::sortModeKey(m)), m);
}

TEST(TabListPanel, Inv10AZListsEveryRowWithNoHeaders) {
    Pages p;
    TabListPanel panel;
    panel.setSortMode(TabListPanel::SortMode::AZ);
    setMixedEntries(panel, p);
    QTreeWidget *tree = panel.treeForTest();
    ASSERT_EQ(tree->topLevelItemCount(), 4) << "A–Z has no colour headers";
    // Case ignored; numbers in number order.
    EXPECT_EQ(rowTitles(tree), (QStringList{"alpha", "Beta", "Tab 2", "tab 10"}));
}

TEST(TabListPanel, Inv10AZKeepsTabOrderBetweenEqualNames) {
    Pages p;
    TabListPanel panel;
    panel.setSortMode(TabListPanel::SortMode::AZ);
    panel.setEntries({{&p.a, QStringLiteral("same"), QColor(), Glyph::None},
                      {&p.b, QStringLiteral("Same"), QColor(), Glyph::None},
                      {&p.c, QStringLiteral("same"), QColor(), Glyph::None}},
                     &p.a);
    const auto rows = tabRows(panel.treeForTest());
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows.at(0)->data(0, Qt::UserRole + 1).value<QObject *>(), &p.a);
    EXPECT_EQ(rows.at(1)->data(0, Qt::UserRole + 1).value<QObject *>(), &p.b);
    EXPECT_EQ(rows.at(2)->data(0, Qt::UserRole + 1).value<QObject *>(), &p.c);
}

TEST(TabListPanel, Inv10TabOrderListsEveryRowWithNoHeaders) {
    Pages p;
    TabListPanel panel;
    panel.setSortMode(TabListPanel::SortMode::TabOrder);
    setMixedEntries(panel, p);
    QTreeWidget *tree = panel.treeForTest();
    ASSERT_EQ(tree->topLevelItemCount(), 4);
    EXPECT_EQ(rowTitles(tree), (QStringList{"tab 10", "Beta", "alpha", "Tab 2"}));
}

TEST(TabListPanel, Inv10ColourAZSortsInsideEachGroup) {
    Pages p;
    TabListPanel panel;
    panel.setSortMode(TabListPanel::SortMode::ByColourAZ);
    setMixedEntries(panel, p);
    QTreeWidget *tree = panel.treeForTest();
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    EXPECT_EQ(tree->topLevelItem(0)->text(0), paletteName(0));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), QStringLiteral("No colour"));
    EXPECT_EQ(rowTitles(tree), (QStringList{"Beta", "Tab 2", "alpha", "tab 10"}));
}

TEST(TabListPanel, Inv10ColourIsTheDefaultAndKeepsTabOrderInsideGroups) {
    Pages p;
    TabListPanel panel;
    setMixedEntries(panel, p);
    EXPECT_EQ(rowTitles(panel.treeForTest()),
              (QStringList{"Beta", "Tab 2", "tab 10", "alpha"}));
}

TEST(TabListPanel, Inv10ChangingTheModeRebuildsTheRows) {
    Pages p;
    TabListPanel panel;
    setMixedEntries(panel, p);
    panel.setSortMode(TabListPanel::SortMode::AZ);
    EXPECT_EQ(rowTitles(panel.treeForTest()),
              (QStringList{"alpha", "Beta", "Tab 2", "tab 10"}));
}

TEST(TabListPanel, Inv10SortKeyRoundTripsThroughConfig) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    guard.setEnv("XDG_CONFIG_HOME", tmp.path().toUtf8());
    {
        Config c;
        EXPECT_EQ(c.tabListPanelSort(), QStringLiteral("colour"));
        c.setTabListPanelSort(QStringLiteral("az"));
    }
    Config again;
    EXPECT_EQ(again.tabListPanelSort(), QStringLiteral("az"));
}

TEST(TabListPanel, Inv10ReloadPassesTheSortToThePanel) {
    const std::string src = ants_test::slurpMainWindow();
    ASSERT_FALSE(src.empty());
    const std::string apply =
        ants_test::slurpFunctionBody(src, "void MainWindow::applyTabListPanelConfig(");
    ASSERT_FALSE(apply.empty());
    EXPECT_NE(apply.find("setSortMode("), std::string::npos);
    EXPECT_NE(apply.find("tabListPanelSort()"), std::string::npos);
}
