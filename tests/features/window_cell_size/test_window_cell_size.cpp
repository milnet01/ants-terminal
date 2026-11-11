// ANTS-6742 — the window's size in columns × rows. Contract: spec.md beside
// this file. INV-1 round-trips Config in a temp XDG_CONFIG_HOME; INV-2 and
// INV-3 drive a TerminalWidget offscreen; INV-4..6 read the MainWindow and
// SettingsDialog sources (a MainWindow cannot be built headless here).

#include "config.h"
#include "terminalgrid.h"
#include "terminalwidget.h"

#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <string>

namespace {

std::string body(const std::string &src, const char *signature) {
    return ants_test::slurpFunctionBody(src, signature);
}

}  // namespace

TEST(WindowCellSize, Inv1ConfigKeysRoundTripWithinBounds) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    guard.setEnv("XDG_CONFIG_HOME", tmp.path().toUtf8());
    {
        Config c;
        EXPECT_FALSE(c.windowFixedSize());
        EXPECT_EQ(c.windowColumns(), 120);
        EXPECT_EQ(c.windowRows(), 36);
        c.setWindowFixedSize(true);
        c.setWindowColumns(200);
        c.setWindowRows(50);
    }
    {
        Config again;
        EXPECT_TRUE(again.windowFixedSize());
        EXPECT_EQ(again.windowColumns(), 200);
        EXPECT_EQ(again.windowRows(), 50);
        again.setWindowColumns(5);
        again.setWindowRows(100000);
        EXPECT_EQ(again.windowColumns(), 20);
        EXPECT_EQ(again.windowRows(), 500);
    }
}

TEST(WindowCellSize, Inv2SizeForGridGivesThatGrid) {
    TerminalWidget w;
    w.show();
    QCoreApplication::processEvents();
    for (const auto &[cols, rows] : {std::pair{200, 50}, std::pair{80, 24}, std::pair{133, 41}}) {
        w.resize(w.sizeForGrid(cols, rows));
        QCoreApplication::processEvents();
        w.forceRecalcSize();
        EXPECT_EQ(w.grid()->cols(), cols);
        EXPECT_EQ(w.grid()->rows(), rows);
    }
}

TEST(WindowCellSize, Inv3OneHelperForTheMargins) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALWIDGET_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string recalc = body(src, "void TerminalWidget::recalcGridSize(");
    const std::string forGrid = body(src, "QSize TerminalWidget::sizeForGrid(");
    ASSERT_FALSE(recalc.empty());
    ASSERT_FALSE(forGrid.empty());
    EXPECT_NE(recalc.find("gridMargins()"), std::string::npos);
    EXPECT_NE(forGrid.find("gridMargins()"), std::string::npos);
    EXPECT_EQ(recalc.find("m_padding"), std::string::npos)
        << "recalcGridSize sums the margins itself again";
}

TEST(WindowCellSize, Inv4ResizeMovesTheTerminalShareOnly) {
    const std::string src = ants_test::slurpMainWindow();
    const std::string fn = body(src, "void MainWindow::resizeToCells(");
    ASSERT_FALSE(fn.empty());
    EXPECT_NE(fn.find("m_quakeMode"), std::string::npos) << "quake mode is resized";
    EXPECT_NE(fn.find("sizeForGrid(cols, rows) - t->size()"), std::string::npos)
        << "the window is not resized by the terminal's difference";
}

TEST(WindowCellSize, Inv5FixedSizeAppliesOnShowAndOnlyWhenChanged) {
    const std::string src = ants_test::slurpMainWindow();
    EXPECT_NE(body(src, "void MainWindow::showEvent(").find("applyFixedWindowSize(true)"),
              std::string::npos);
    EXPECT_NE(body(src, "void MainWindow::onConfigFileChanged(").find("applyFixedWindowSize(false)"),
              std::string::npos);
    EXPECT_NE(body(src, "&SettingsDialog::settingsChanged, this, [this]()")
                  .find("applyFixedWindowSize(false)"),
              std::string::npos);
    const std::string apply = body(src, "void MainWindow::applyFixedWindowSize(");
    ASSERT_FALSE(apply.empty());
    EXPECT_NE(apply.find("want == m_appliedFixedSize"), std::string::npos)
        << "an unrelated setting would resize the window";
}

TEST(WindowCellSize, Inv6MenuAndSettingsControls) {
    const std::string src = ants_test::slurpMainWindow();
    EXPECT_NE(src.find("\"&Resize Window...\""), std::string::npos);
    EXPECT_NE(src.find("&MainWindow::showResizeWindowDialog"), std::string::npos);
    const std::string dialog =
        ants_test::slurpFile(SRC_SETTINGSDIALOG_CPP_PATH);
    ASSERT_FALSE(dialog.empty());
    const std::string load = body(dialog, "void SettingsDialog::loadSettings(");
    const std::string save = body(dialog, "void SettingsDialog::applySettings(");
    EXPECT_NE(load.find("windowFixedSize()"), std::string::npos);
    EXPECT_NE(load.find("windowColumns()"), std::string::npos);
    EXPECT_NE(load.find("windowRows()"), std::string::npos);
    EXPECT_NE(save.find("setWindowFixedSize("), std::string::npos);
    EXPECT_NE(save.find("setWindowColumns("), std::string::npos);
    EXPECT_NE(save.find("setWindowRows("), std::string::npos);
}
