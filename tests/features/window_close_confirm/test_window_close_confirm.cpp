// Feature-conformance test for ANTS-5120 — closing a window must ask before
// ending the programs running in its tabs, the way a single tab close does.
// Pure source-grep (no link, no MainWindow construction — see spec.md's
// "Why source-grep" section).
//
// INV labels: ANTS-5120-INV-N. See spec.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

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

TEST(WindowCloseConfirm, Inv1_closeEventChecksAndIgnoresBeforeSaveAndDeferral) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5120-INV-1: read mainwindow.cpp (" << SRC_MAINWINDOW_CPP_PATH << ")";
    // Comment-stripped so a documentation comment describing the fix can't
    // satisfy this the wrong way. Anchored at `void MainWindow::` because
    // slurpFunctionBody takes the FIRST match, and a bare
    // `MainWindow::closeEvent` could first match a connect() call.
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body = ants_test::slurpFunctionBody(stripped, "void MainWindow::closeEvent(");
    expect(!body.empty(), "ANTS-5120-INV-1: MainWindow::closeEvent body extracted");

    const auto npos = std::string::npos;
    const auto confirmPos = body.find("confirmCloseWithProcesses()");
    const auto descendantPos = body.find("firstNonShellDescendant(");
    const auto ignorePos = body.find("event->ignore()");
    const auto saveAllSessionsPos = body.find("saveAllSessions()");
    const auto anotherWindowPos = body.find("anotherWindowStaysOpen()");

    expect(confirmPos != npos,
           "ANTS-5120-INV-1: closeEvent checks m_config.confirmCloseWithProcesses()");
    expect(descendantPos != npos,
           "ANTS-5120-INV-1: closeEvent calls firstNonShellDescendant to probe for a running "
           "program — without this, closing a window never asks");
    expect(ignorePos != npos,
           "ANTS-5120-INV-1: closeEvent calls event->ignore() on a hit, so the window stays open "
           "for the dialog's answer instead of finishing the close silently");
    ASSERT_TRUE(saveAllSessionsPos != npos) << "ANTS-5120-INV-1: closeEvent still calls "
        "saveAllSessions() (ANTS-1159 regression: this must survive the fix, not be replaced)";
    ASSERT_TRUE(anotherWindowPos != npos) << "ANTS-5120-INV-1: closeEvent still calls "
        "anotherWindowStaysOpen() (ANTS-5118 regression: this must survive the fix)";

    expect(ignorePos != npos && ignorePos < saveAllSessionsPos,
           "ANTS-5120-INV-1: event->ignore() happens before saveAllSessions() — a confirmed-later "
           "close must not have already saved and torn down sessions on the first, cancelled pass");
    expect(ignorePos != npos && ignorePos < anotherWindowPos,
           "ANTS-5120-INV-1: event->ignore() happens before the ANTS-5118 deferred close-down "
           "(anotherWindowStaysOpen()) — that deferral must not fire on the pass the user is "
           "about to cancel");
    expect(confirmPos != npos && descendantPos != npos && ignorePos != npos
               && confirmPos < ignorePos && descendantPos < ignorePos,
           "ANTS-5120-INV-1: the confirmCloseWithProcesses()/firstNonShellDescendant() check "
           "happens before event->ignore() is called — the ignore is the check's consequence, "
           "not a step ahead of it");
    EXPECT_EQ(0, expect_failures())
        << "Inv1_closeEventChecksAndIgnoresBeforeSaveAndDeferral failed";
}

TEST(WindowCloseConfirm, Inv2_closeEventChecksEveryLiveTerminal) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5120-INV-2: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body = ants_test::slurpFunctionBody(stripped, "void MainWindow::closeEvent(");
    expect(!body.empty(), "ANTS-5120-INV-2: MainWindow::closeEvent body extracted");

    expect(contains(body, "liveTerminals()"),
           "ANTS-5120-INV-2: closeEvent's running-program check walks liveTerminals() — every "
           "pane in the window, not only the active pane of the current tab. A background split "
           "running vim must still trigger the question");
    EXPECT_EQ(0, expect_failures()) << "Inv2_closeEventChecksEveryLiveTerminal failed";
}

TEST(WindowCloseConfirm, Inv3_windowDialogIsNonModal) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5120-INV-3: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void MainWindow::showCloseWindowConfirmDialog(");
    // Not yet defined against the current tree — this is expected to fail
    // until the fix adds the helper (see spec.md's Scope on the name).
    expect(!body.empty(),
           "ANTS-5120-INV-3: MainWindow::showCloseWindowConfirmDialog body extracted — the fix "
           "must define this helper (spec.md names the exact signature this test anchors on)");

    expect(!contains(body, "exec("),
           "ANTS-5120-INV-3: showCloseWindowConfirmDialog does not call exec() — a modal dialog "
           "would deadlock the Wayland-correct non-modal pattern this app uses everywhere else");
    expect(!contains(body, "setModal(true)"),
           "ANTS-5120-INV-3: showCloseWindowConfirmDialog does not call setModal(true)");
    expect(contains(body, "show()"),
           "ANTS-5120-INV-3: showCloseWindowConfirmDialog calls show() — a non-modal dialog only "
           "protects Wayland if it is actually shown non-modally, not exec()'d");
    EXPECT_EQ(0, expect_failures()) << "Inv3_windowDialogIsNonModal failed";
}

TEST(WindowCloseConfirm, Inv4_proceedPathClosesWindowAndSetsFlagCloseEventReads) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5120-INV-4: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string dialogBody = ants_test::slurpFunctionBody(
        stripped, "void MainWindow::showCloseWindowConfirmDialog(");
    expect(!dialogBody.empty(),
           "ANTS-5120-INV-4: MainWindow::showCloseWindowConfirmDialog body extracted");

    // A call to close() on the WINDOW, not just the dialog (`dlg->close()`).
    // Scan every "close();" occurrence and accept one not immediately
    // preceded by "dlg->".
    bool windowCloseCall = false;
    if (!dialogBody.empty()) {
        std::size_t p = 0;
        while ((p = dialogBody.find("close();", p)) != std::string::npos) {
            const bool viaDlg = (p >= 5) && dialogBody.compare(p - 5, 5, "dlg->") == 0;
            if (!viaDlg) { windowCloseCall = true; break; }
            p += 8;
        }
    }
    expect(windowCloseCall,
           "ANTS-5120-INV-4: showCloseWindowConfirmDialog's proceed path calls close() on the "
           "window itself (not only dlg->close()) — otherwise the close never resumes and the "
           "window can never actually be closed");

    // A member-flag assignment: `m_<ident> = true;` (allowing whitespace),
    // read back the identifier so we can confirm closeEvent consults it too.
    std::string flagName;
    if (!dialogBody.empty()) {
        std::size_t p = 0;
        while ((p = dialogBody.find("m_", p)) != std::string::npos) {
            std::size_t e = p + 2;
            while (e < dialogBody.size()
                   && (std::isalnum(static_cast<unsigned char>(dialogBody[e])) || dialogBody[e] == '_'))
                ++e;
            std::size_t q = e;
            while (q < dialogBody.size() && std::isspace(static_cast<unsigned char>(dialogBody[q])))
                ++q;
            if (q < dialogBody.size() && dialogBody[q] == '='
                    && !(q + 1 < dialogBody.size() && dialogBody[q + 1] == '=')) {
                std::size_t r = q + 1;
                while (r < dialogBody.size() && std::isspace(static_cast<unsigned char>(dialogBody[r])))
                    ++r;
                if (dialogBody.compare(r, 4, "true") == 0) {
                    flagName = dialogBody.substr(p, e - p);
                    break;
                }
            }
            p = e;
        }
    }
    expect(!flagName.empty(),
           "ANTS-5120-INV-4: showCloseWindowConfirmDialog's proceed path sets an m_-prefixed "
           "member flag to true — closeEvent needs something to consult so Close anyway does not "
           "loop back into the same dialog");

    if (!flagName.empty()) {
        const std::string closeEventBody =
            ants_test::slurpFunctionBody(stripped, "void MainWindow::closeEvent(");
        expect(!closeEventBody.empty() && contains(closeEventBody, flagName),
               "ANTS-5120-INV-4: closeEvent reads the same flag the dialog's proceed path "
               "sets — a flag set nowhere read is not a guard",
               "flag: " + flagName);
    }
    EXPECT_EQ(0, expect_failures())
        << "Inv4_proceedPathClosesWindowAndSetsFlagCloseEventReads failed";
}

TEST(WindowCloseConfirm, Inv5_dontAskAgainWritesSameConfigSettingAsTabClose) {
    expect_reset();
    const std::string cp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(cp.empty()) << "ANTS-5120-INV-5: read mainwindow.cpp";
    const std::string stripped = ants_test::stripComments(cp);

    const std::string body = ants_test::slurpFunctionBody(
        stripped, "void MainWindow::showCloseWindowConfirmDialog(");
    expect(!body.empty(),
           "ANTS-5120-INV-5: MainWindow::showCloseWindowConfirmDialog body extracted");

    expect(contains(body, "setConfirmCloseWithProcesses(false)"),
           "ANTS-5120-INV-5: showCloseWindowConfirmDialog's don't-ask-again path calls "
           "m_config.setConfirmCloseWithProcesses(false) — the same setting closeTab's dialog "
           "uses, so the choice applies to both tab close and window close consistently");
    EXPECT_EQ(0, expect_failures()) << "Inv5_dontAskAgainWritesSameConfigSettingAsTabClose failed";
}

}  // namespace
