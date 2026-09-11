// Feature-conformance test for ANTS-5032 — multi-window session-file
// wiring. Pure source-grep (no link, no MainWindow construction — see
// spec.md's "Why source-grep" section).
//
// INV labels qualified ANTS-5032-INV-N. See spec.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <gtest/gtest.h>

#ifndef SRC_MAINWINDOW_H_PATH
#  error "SRC_MAINWINDOW_H_PATH must be defined by the bundle's compile defs"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH must be defined by the bundle's compile defs"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

TEST(MultiWindowSession, Inv1_restoreSessionsHasOnceGuardBeforeLoad) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-1: read mainwindow.cpp (" << SRC_MAINWINDOW_CPP_PATH << ")";
    // Comment-stripped so a documentation comment mentioning "static bool"
    // or "loadTabOrder" can't satisfy this the wrong way. Every anchor in
    // this file starts at `void MainWindow::` because slurpFunctionBody
    // takes the FIRST match, and a bare `MainWindow::saveAllSessions`
    // first matches the constructor's connect() call.
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body = ants_test::slurpFunctionBody(stripped, "void MainWindow::restoreSessions(");
    expect(!body.empty(), "ANTS-5032-INV-1: MainWindow::restoreSessions body extracted");

    const auto staticPos = body.find("static bool");
    const auto loadPos = body.find("loadTabOrder");
    expect(staticPos != std::string::npos,
           "ANTS-5032-INV-1: restoreSessions declares a function-local `static bool` guard");
    expect(loadPos != std::string::npos,
           "ANTS-5032-INV-1: restoreSessions calls SessionManager::loadTabOrder");
    expect(staticPos != std::string::npos && loadPos != std::string::npos && staticPos < loadPos,
           "ANTS-5032-INV-1: the static bool guard is checked before loadTabOrder, so a second "
           "MainWindow constructed in the same process returns early instead of re-opening "
           "every saved tab under ids the first window already owns");

    // A `static bool` that is never tested, or never set true, guards
    // nothing — so both must happen between the declaration and the load.
    bool testedAndSet = false;
    if (staticPos != std::string::npos && loadPos != std::string::npos && staticPos < loadPos) {
        std::size_t p = staticPos + std::string("static bool").size();
        while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
        std::size_t e = p;
        while (e < body.size()
               && (std::isalnum(static_cast<unsigned char>(body[e])) || body[e] == '_'))
            ++e;
        const std::string name = body.substr(p, e - p);
        const std::string guardSpan = body.substr(staticPos, loadPos - staticPos);
        testedAndSet = !name.empty()
            && contains(guardSpan, "if (" + name + ") return;")
            && contains(guardSpan, name + " = true;");
    }
    expect(testedAndSet,
           "ANTS-5032-INV-1: the guard both returns early when set and is set true, before "
           "loadTabOrder");
    EXPECT_EQ(0, expect_failures()) << "Inv1_restoreSessionsHasOnceGuardBeforeLoad failed";
}

TEST(MultiWindowSession, Inv2_exactlyOneSaveTabOrderCallSite) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-2: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::size_t count =
        ants_test::countOccurrences(stripped, "SessionManager::saveTabOrder(");
    const std::string found = "found " + std::to_string(count);
    expect(count == 1,
           "ANTS-5032-INV-2: mainwindow.cpp calls SessionManager::saveTabOrder( exactly once — "
           "two call sites means two windows each decide tab_order.txt from only their own "
           "tabs, and the last one to save wins",
           found.c_str());
    EXPECT_EQ(0, expect_failures()) << "Inv2_exactlyOneSaveTabOrderCallSite failed";
}

TEST(MultiWindowSession, Inv3_saveTabOrderCallSiteInsideSaveProcessTabOrder) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-3: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body =
        ants_test::slurpFunctionBody(stripped, "void MainWindow::saveProcessTabOrder(");
    expect(!body.empty(), "ANTS-5032-INV-3: MainWindow::saveProcessTabOrder body extracted");
    expect(contains(body, "SessionManager::saveTabOrder("),
           "ANTS-5032-INV-3: saveProcessTabOrder is the function that calls "
           "SessionManager::saveTabOrder(");
    EXPECT_EQ(0, expect_failures()) << "Inv3_saveTabOrderCallSiteInsideSaveProcessTabOrder failed";
}

TEST(MultiWindowSession, Inv4_saveProcessTabOrderWalksTopLevelWidgets) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-4: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body =
        ants_test::slurpFunctionBody(stripped, "void MainWindow::saveProcessTabOrder(");
    expect(!body.empty(), "ANTS-5032-INV-4: MainWindow::saveProcessTabOrder body extracted");
    expect(contains(body, "QApplication::topLevelWidgets()"),
           "ANTS-5032-INV-4: saveProcessTabOrder walks QApplication::topLevelWidgets() to find "
           "every other open MainWindow in the process");
    EXPECT_EQ(0, expect_failures()) << "Inv4_saveProcessTabOrderWalksTopLevelWidgets failed";
}

TEST(MultiWindowSession, Inv5_saveProcessTabOrderCastsToMainWindowAndSkipsThis) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-5: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body =
        ants_test::slurpFunctionBody(stripped, "void MainWindow::saveProcessTabOrder(");
    expect(!body.empty(), "ANTS-5032-INV-5: MainWindow::saveProcessTabOrder body extracted");

    // qobject_cast<...MainWindow *>(...) — allow "const MainWindow" or
    // "MainWindow", cv-qualifiers either side of the cast target.
    const auto castPos = body.find("qobject_cast");
    bool castsToMainWindow = false;
    if (castPos != std::string::npos) {
        const std::size_t hi = std::min(castPos + 80, body.size());
        castsToMainWindow = contains(body.substr(castPos, hi - castPos), "MainWindow");
    }
    expect(castsToMainWindow,
           "ANTS-5032-INV-5: saveProcessTabOrder qobject_casts each top-level widget to "
           "MainWindow (not just any QWidget) before folding its tabs in");
    // The exclusion is a comparison against `this` after the cast, not a
    // bare mention of `this` anywhere in the body.
    const std::string afterCast =
        castPos == std::string::npos ? std::string() : body.substr(castPos);
    expect(contains(afterCast, "!= this") || contains(afterCast, "== this"),
           "ANTS-5032-INV-5: saveProcessTabOrder excludes `this` from the other-window walk — "
           "a hidden Quake-mode window still owns tabs and must still be folded in, so only "
           "`this` is skipped, not every non-visible widget");
    EXPECT_EQ(0, expect_failures()) << "Inv5_saveProcessTabOrderCastsToMainWindowAndSkipsThis failed";
}

TEST(MultiWindowSession, Inv6_saveAllSessionsCallsSaveProcessTabOrder) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-6: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body = ants_test::slurpFunctionBody(stripped, "void MainWindow::saveAllSessions(");
    expect(!body.empty(), "ANTS-5032-INV-6: MainWindow::saveAllSessions body extracted");
    expect(contains(body, "saveProcessTabOrder"),
           "ANTS-5032-INV-6: saveAllSessions ends by calling saveProcessTabOrder instead of "
           "SessionManager::saveTabOrder directly");
    EXPECT_EQ(0, expect_failures()) << "Inv6_saveAllSessionsCallsSaveProcessTabOrder failed";
}

TEST(MultiWindowSession, Inv7_saveTabOrderOnlyCallsSaveProcessTabOrder) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-7: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body = ants_test::slurpFunctionBody(stripped, "void MainWindow::saveTabOrderOnly(");
    expect(!body.empty(), "ANTS-5032-INV-7: MainWindow::saveTabOrderOnly body extracted");
    expect(contains(body, "saveProcessTabOrder"),
           "ANTS-5032-INV-7: saveTabOrderOnly ends by calling saveProcessTabOrder instead of "
           "SessionManager::saveTabOrder directly");
    EXPECT_EQ(0, expect_failures()) << "Inv7_saveTabOrderOnlyCallsSaveProcessTabOrder failed";
}

TEST(MultiWindowSession, Inv8_saveProcessTabOrderAppendsOtherWindowsTabs) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-8: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body =
        ants_test::slurpFunctionBody(stripped, "void MainWindow::saveProcessTabOrder(");
    expect(!body.empty(), "ANTS-5032-INV-8: MainWindow::saveProcessTabOrder body extracted");

    // The statement that reads another window's sessionTabIds() must add
    // them to the list being saved — reading them and discarding them
    // walks every window and still saves only this one's tabs.
    const auto castPos = body.find("qobject_cast");
    const auto idsPos = castPos == std::string::npos
        ? std::string::npos : body.find("sessionTabIds(", castPos);
    bool appended = false;
    if (idsPos != std::string::npos) {
        const auto stmtStart = body.find_last_of(";{", idsPos);
        const auto stmtEnd = body.find(';', idsPos);
        if (stmtStart != std::string::npos && stmtEnd != std::string::npos) {
            const std::string stmt = body.substr(stmtStart + 1, stmtEnd - stmtStart - 1);
            appended = contains(stmt, "+=") || contains(stmt, "append(") || contains(stmt, "<<");
        }
    }
    expect(appended,
           "ANTS-5032-INV-8: saveProcessTabOrder adds each other window's sessionTabIds() to "
           "the list it saves");
    EXPECT_EQ(0, expect_failures()) << "Inv8_saveProcessTabOrderAppendsOtherWindowsTabs failed";
}

TEST(MultiWindowSession, Inv9_saveProcessTabOrderIgnoresVisibility) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5032-INV-9: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body =
        ants_test::slurpFunctionBody(stripped, "void MainWindow::saveProcessTabOrder(");
    expect(!body.empty(), "ANTS-5032-INV-9: MainWindow::saveProcessTabOrder body extracted");
    expect(!contains(body, "isVisible") && !contains(body, "isHidden"),
           "ANTS-5032-INV-9: saveProcessTabOrder does not filter windows on visibility — a "
           "Quake-mode window hides but still owns its tabs");
    EXPECT_EQ(0, expect_failures()) << "Inv9_saveProcessTabOrderIgnoresVisibility failed";
}

}  // namespace
