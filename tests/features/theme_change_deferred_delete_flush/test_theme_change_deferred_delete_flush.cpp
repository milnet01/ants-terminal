// ANTS-2024 — regression guard for the SIGSEGV-on-theme-change fix.
// Source-scrape over mainwindow.cpp asserting applyTheme reaps pending
// DeferredDelete events BEFORE the app-wide qApp->setStyleSheet walk, so
// a deleteLater()'d Claude permission-prompt widget can't be left
// dangling in the widget set Qt iterates during the restyle. The runtime
// use-after-free is confirmed manually under the ASan debug build (open a
// permission prompt, then change theme); this test stops the ordering
// from silently regressing.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef ANTS_MAINWINDOW_SOURCES
#error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif

namespace {


}  // namespace

// INV-1 / INV-2 — the DeferredDelete flush precedes the app-wide restyle
// inside MainWindow::applyTheme, and both anchors are present.
TEST(theme_change_deferred_delete_flush, FlushPrecedesAppWideSetStyleSheet) {
    const std::string src = ants_test::slurpMainWindow();
    ASSERT_FALSE(src.empty()) << "could not read mainwindow.cpp";

    // ANTS-1677 — bound the search to the applyTheme definition's body.
    const std::string body =
        ants_test::slurpFunctionBody(src, "void MainWindow::applyTheme(");
    ASSERT_FALSE(body.empty()) << "MainWindow::applyTheme body not found";

    const auto flushPos = body.find(
        "sendPostedEvents(nullptr, QEvent::DeferredDelete)");
    // Anchor on the real call's argument (buildAppStylesheet), NOT a bare
    // "qApp->setStyleSheet(" — ANTS-2097 added an explanatory comment that
    // mentions `qApp->setStyleSheet()` near the top of applyTheme, and a
    // bare substring match would latch onto that comment (ahead of the
    // flush) and false-fail this ordering check.
    const auto restylePos = body.find(
        "qApp->setStyleSheet(themedstylesheet::buildAppStylesheet");

    // INV-2 — both anchors present (so INV-1 isn't trivially satisfied by
    // two absent substrings).
    ASSERT_NE(flushPos, std::string::npos)
        << "INV-2: applyTheme must reap DeferredDelete before the restyle "
           "(sendPostedEvents(nullptr, QEvent::DeferredDelete) missing)";
    ASSERT_NE(restylePos, std::string::npos)
        << "INV-2: applyTheme must call qApp->setStyleSheet( (app-wide "
           "restyle anchor missing)";

    // INV-1 — ordering: flush before the walk.
    EXPECT_LT(flushPos, restylePos)
        << "INV-1: the DeferredDelete flush must precede "
           "qApp->setStyleSheet() so a deleteLater()'d permission-prompt "
           "widget is fully reaped before Qt's app-wide widget walk "
           "(ANTS-2024 SIGSEGV)";
}
