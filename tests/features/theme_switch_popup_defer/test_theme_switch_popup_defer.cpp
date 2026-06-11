// Feature-conformance test for ANTS-2097 — theme switch must defer the
// app-wide restyle out of a popup menu's nested event loop, so
// QApplication::setStyleSheet doesn't walk a freed widget pointer.
// See tests/features/theme_switch_popup_defer/spec.md.
//
// Source-grep against MainWindow::applyTheme: the activePopupWidget()
// guard + singleShot(0) deferral must gate the qApp->setStyleSheet walk.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {

// Slice of mainwindow.cpp covering the body of MainWindow::applyTheme,
// from its definition to the next top-level MainWindow:: method.
std::string applyThemeBody() {
    const std::string src = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const auto defPos = src.find("void MainWindow::applyTheme(");
    EXPECT_NE(defPos, std::string::npos);
    const auto nextDef = src.find("\nvoid MainWindow::", defPos + 20);
    return src.substr(defPos, nextDef == std::string::npos
                                  ? src.size() - defPos
                                  : nextDef - defPos);
}

}  // namespace

// INV-1 — the popup guard gates the restyle: activePopupWidget() is
// referenced, and it appears before the qApp->setStyleSheet(themedstylesheet:: call.
TEST(ThemeSwitchPopupDefer, Inv1GuardPrecedesRestyle) {
    const std::string body = applyThemeBody();
    const auto guardPos  = body.find("activePopupWidget()");
    const auto restylePos = body.find("qApp->setStyleSheet(themedstylesheet::");
    ASSERT_NE(guardPos, std::string::npos)
        << "applyTheme must consult QApplication::activePopupWidget()";
    ASSERT_NE(restylePos, std::string::npos)
        << "applyTheme must call qApp->setStyleSheet(themedstylesheet::)";
    EXPECT_LT(guardPos, restylePos)
        << "the popup guard must gate the restyle, not follow it";
}

// INV-2 — the guard defers via singleShot(0) and returns.
TEST(ThemeSwitchPopupDefer, Inv2DefersViaSingleShot) {
    const std::string body = applyThemeBody();
    const auto guardPos = body.find("activePopupWidget()");
    ASSERT_NE(guardPos, std::string::npos);
    // The singleShot(0) deferral must sit just after the guard.
    const auto deferPos = body.find("QTimer::singleShot(0", guardPos);
    ASSERT_NE(deferPos, std::string::npos)
        << "the popup branch must re-post applyTheme via singleShot(0)";
    // And it must re-enter applyTheme (deferred recursion).
    EXPECT_NE(body.find("applyTheme(", deferPos), std::string::npos);
    // A return must end the deferred branch before the synchronous body.
    const auto retPos = body.find("return;", deferPos);
    const auto restylePos = body.find("qApp->setStyleSheet(themedstylesheet::");
    ASSERT_NE(retPos, std::string::npos);
    EXPECT_LT(retPos, restylePos)
        << "the popup branch must return before the synchronous restyle";
}

// INV-3 — the same-theme early-return precedes the popup guard, so a
// no-op re-select neither defers nor restyles.
TEST(ThemeSwitchPopupDefer, Inv3EarlyReturnFirst) {
    const std::string body = applyThemeBody();
    const auto earlyPos = body.find("name == m_currentTheme");
    const auto guardPos = body.find("activePopupWidget()");
    ASSERT_NE(earlyPos, std::string::npos);
    ASSERT_NE(guardPos, std::string::npos);
    EXPECT_LT(earlyPos, guardPos)
        << "same-theme early-return must precede the popup guard";
}
