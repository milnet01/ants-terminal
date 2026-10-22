// ANTS-5646 — go to a tab by typing its name. Contract: spec.md beside
// this file. INV-1..5 drive CommandPalette offscreen; INV-3's deferral and
// INV-6 read the MainWindow and Settings sources.

#include "commandpalette.h"
#include "tablistpanel.h"

#include "../../_support/srcgrep.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QWidget>

#include <memory>

#include <gtest/gtest.h>

namespace {

struct Fixture {
    QWidget host;
    CommandPalette *palette = new CommandPalette(&host);
    std::vector<std::unique_ptr<QAction>> actions;
    QStringList fired;

    Fixture() { host.resize(1000, 700); }

    QList<QAction *> tabs(const QStringList &titles) {
        QList<QAction *> out;
        for (const QString &t : titles) {
            auto a = std::make_unique<QAction>(t);
            QPixmap pm(8, 8);
            pm.fill(Qt::red);
            a->setIcon(QIcon(pm));
            QObject::connect(a.get(), &QAction::triggered, [this, t] { fired << t; });
            out << a.get();
            actions.push_back(std::move(a));
        }
        return out;
    }

    QStringList rows() const {
        QStringList out;
        QListWidget *l = palette->listForTest();
        for (int i = 0; l && i < l->count(); ++i) out << l->item(i)->text();
        return out;
    }
};

std::string mainWindowSource() { return ants_test::slurpMainWindow(); }

}  // namespace

TEST(TabSwitcher, Inv1OneRowPerTabWithTextAsWritten) {
    Fixture f;
    f.palette->showTabs(f.tabs({QStringLiteral("Lotto & Draws"), QStringLiteral("RetroDB")}), 16);
    EXPECT_TRUE(f.palette->inTabMode());
    EXPECT_EQ(f.rows(), (QStringList{QStringLiteral("Lotto & Draws"), QStringLiteral("RetroDB")}));
}

TEST(TabSwitcher, Inv2TypingFiltersIgnoringCase) {
    Fixture f;
    f.palette->showTabs(f.tabs({QStringLiteral("Paperkist"), QStringLiteral("RetroArch"),
                                QStringLiteral("RetroDB")}),
                        16);
    f.palette->inputForTest()->setText(QStringLiteral("retro"));
    EXPECT_EQ(f.rows(), (QStringList{QStringLiteral("RetroArch"), QStringLiteral("RetroDB")}));
    ASSERT_NE(f.palette->listForTest(), nullptr);
    EXPECT_EQ(f.palette->listForTest()->currentRow(), 0);
}

TEST(TabSwitcher, Inv3EnterTriggersTheSelectedTab) {
    Fixture f;
    f.palette->showTabs(f.tabs({QStringLiteral("Paperkist"), QStringLiteral("RetroDB")}), 16);
    f.palette->inputForTest()->setText(QStringLiteral("db"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(f.palette->inputForTest(), &enter);
    EXPECT_EQ(f.fired, QStringList{QStringLiteral("RetroDB")});
    EXPECT_FALSE(f.palette->inTabMode());
}

TEST(TabSwitcher, Inv3MainWindowDefersTheTabActionCleanup) {
    const std::string src = mainWindowSource();
    ASSERT_FALSE(src.empty());
    const std::string closed =
        ants_test::slurpFunctionBody(src, "&CommandPalette::closed, this, [this]()");
    ASSERT_FALSE(closed.empty());
    EXPECT_NE(closed.find("QTimer::singleShot"), std::string::npos)
        << "the tab actions would be deleted before Enter fires them";
    EXPECT_NE(closed.find("rebuildCommandPalette()"), std::string::npos);
}

TEST(TabSwitcher, Inv4RowsAreReadableAndCarryTheIcon) {
    Fixture f;
    f.palette->showTabs(f.tabs({QStringLiteral("Vestige")}), 21);
    QListWidget *list = f.palette->listForTest();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 1);
    EXPECT_EQ(list->font().pointSize(), 21);
    EXPECT_EQ(f.palette->inputForTest()->font().pointSize(), 21);
    EXPECT_FALSE(list->item(0)->icon().isNull());

    TabListEntry e;
    e.title = QStringLiteral("x");
    e.color = Qt::blue;
    e.glyph = ClaudeTabIndicator::Glyph::Thinking;
    EXPECT_FALSE(TabListPanel::tabIcon(e, 20, Qt::black, 1.0).isNull());
}

TEST(TabSwitcher, Inv5ClosingReturnsToCommandMode) {
    Fixture f;
    const QString commandPlaceholder = f.palette->inputForTest()->placeholderText();
    const int commandPoints = f.palette->inputForTest()->font().pointSize();
    f.palette->showTabs(f.tabs({QStringLiteral("Snatch")}), 24);
    EXPECT_NE(f.palette->inputForTest()->placeholderText(), commandPlaceholder);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(f.palette->inputForTest(), &esc);
    EXPECT_FALSE(f.palette->inTabMode());
    EXPECT_EQ(f.palette->inputForTest()->placeholderText(), commandPlaceholder);
    EXPECT_EQ(f.palette->inputForTest()->font().pointSize(), commandPoints);
}

TEST(TabSwitcher, Inv6MenuAndShortcutTable) {
    const std::string src = mainWindowSource();
    EXPECT_NE(src.find("\"Go to &Tab...\""), std::string::npos);
    EXPECT_NE(src.find("keybinding(\"tab_switcher\""), std::string::npos);
    const std::string config = ants_test::slurpFile(SRC_CONFIG_CPP_PATH);
    ASSERT_FALSE(config.empty());
    std::size_t count = 0;
    for (std::size_t at = config.find("{\"tab_switcher\""); at != std::string::npos;
         at = config.find("{\"tab_switcher\"", at + 1))
        ++count;
    EXPECT_EQ(count, 1u) << "the one shortcut table lists tab_switcher";
}
