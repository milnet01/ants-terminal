// Feature-conformance test for spec.md — pins the ANTS-1375 fix.
// Source-grep only: behavioural exercise of restoreSessions requires
// a full MainWindow + session-persistence fixture, which is heavier
// than the test_core bundle can carry. The grep here catches a
// regression that re-introduces the missing trackShell call.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <string>

#include <gtest/gtest.h>

#ifndef ANTS_MAINWINDOW_SOURCES
#error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

void testWiring() {
    const std::string mwc = ants_test::slurpMainWindow();
    const std::string body =
        ants_test::slurpFunctionBody(mwc, "MainWindow::restoreSessions");
    expect(!body.empty(),
           "anchor: MainWindow::restoreSessions body slurped");
    if (body.empty()) return;

    // WI-1 — restoreSessions calls trackShell on each restored tab.
    // Without this, ClaudeTabTracker::m_shells is missing the
    // restored shell PIDs and shellState() returns NotRunning →
    // no per-tab Claude dot.
    expect(body.find("trackShell(") != std::string::npos,
           "WI-1 restoreSessions calls trackShell on restored shells");

    // WI-2 — the body still starts shells (defensive against
    // accidental hoist of trackShell out of the loop where it must
    // run AFTER startShell has set the shell PID).
    expect(body.find("startShell(") != std::string::npos,
           "WI-2 restoreSessions still calls startShell");

    // WI-3 (ANTS-5733) — once the shells run, the focused tab's shell
    // PID reaches ClaudeIntegration. The tab is made current before its
    // shell starts, so the tab-switch handler saw PID 0 and stopped the
    // poll; without this the context gauge never appeared after a
    // relaunch until the user switched tabs.
    const size_t start = body.find("startShell(");
    const size_t wire = body.find("setShellPid(");
    expect(wire != std::string::npos && start != std::string::npos
               && wire > start,
           "WI-3 restoreSessions calls setShellPid after startShell");
}

int runMain() {
    expect_reset();
    testWiring();
    return expect_finish();
}

}  // namespace

TEST(ClaudeDotRestoredTabs, Main) { ASSERT_EQ(0, runMain()); }
