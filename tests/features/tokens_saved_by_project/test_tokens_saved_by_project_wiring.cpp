// ANTS-3579 — source-scrape wiring tests for the per-project tokens-saved pill.
// The pure fold/prune math is in test_tokens_saved_by_project.cpp (test_audit);
// this file (test_claude — has the SRC_*_PATH defines) pins the wiring:
//   INV-1/11 recordDispatch attribution; INV-12 clear-after-fold; INV-2/3/8/10/11
//   the widget render; INV-4/6/7 the MainWindow fold + tab-switch refresh.
// See docs/specs/ANTS-3579.md § 3 / § 5.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH required"
#endif
#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH required"
#endif

ANTS_TEST_SCOPE();

namespace {
bool has(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}
size_t count(const std::string &h, const std::string &n) {
    size_t c = 0, p = 0;
    while ((p = h.find(n, p)) != std::string::npos) { ++c; p += n.size(); }
    return c;
}
}  // namespace

// INV-1 + INV-11 — recordDispatch attributes per-project from caller_cwd, no
// MainWindow / resolveCallerCwdRoot.
TEST(TokensSavedByProjectWiring, Inv1And11RecordDispatch) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string body =
        ants_test::slurpFunctionBody(cpp, "ClaudeIntegration::recordDispatch");
    expect(!body.empty(), "recordDispatch body located");
    expect(has(body, "if (succeeded)"),
           "INV-1: attribution gated on the success flag");
    expect(has(body, "canonicalFilePath"),
           "INV-1/11: root derived via QFileInfo::canonicalFilePath");
    expect(has(body, "baselineFor"), "INV-1: uses baselineFor");
    expect(has(body, "(argBytes + outBytes)"),
           "INV-1: subtracts argBytes + outBytes (matches the engine term)");
    expect(has(body, "m_sessionSavedBytesByProject[root] +="),
           "INV-1: accumulates saved bytes per project root");
    expect(!has(body, "resolveCallerCwdRoot"),
           "INV-11: recordDispatch does NOT use resolveCallerCwdRoot (no MainWindow)");
    EXPECT_EQ(0, expect_failures());
}

// INV-12 — endTokenSession clears the live map AFTER the synchronous fold emit.
TEST(TokensSavedByProjectWiring, Inv12ClearAfterFold) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string body =
        ants_test::slurpFunctionBody(cpp, "ClaudeIntegration::endTokenSession");
    const auto emitPos  = body.find("emit tokenSessionEnding()");
    const auto clearPos = body.find("m_sessionSavedBytesByProject.clear()");
    expect(emitPos != std::string::npos, "INV-12: fold emit present");
    expect(clearPos != std::string::npos, "INV-12: live-map clear present");
    if (emitPos != std::string::npos && clearPos != std::string::npos)
        expect(emitPos < clearPos,
               "INV-12: the live map is cleared AFTER the fold emit (never before)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2/3/8/10/11 — the widget render method scopes to the focused project.
TEST(TokensSavedByProjectWiring, Inv2To11Widget) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body = ants_test::slurpFunctionBody(
        cpp, "ClaudeStatusBarController::refreshTokensSavedChip");
    expect(!body.empty(), "refreshTokensSavedChip body located");
    expect(has(body, "m_focusedTerminalProvider"),
           "M2: reaches the focused tab via the provider, not focusedTerminal()");
    expect(has(body, "canonicalFilePath"), "INV-11: same canonical derivation");
    expect(has(body, "sessionSavedBytesForProject"),
           "INV-2: face reads the focused-project session getter");
    expect(has(body, "claudeTokensSavedByProject"),
           "INV-2/3: reads the per-project persisted store");
    expect(has(body, "kCharsPerToken"),
           "INV-8: bytes→tokens division by the public symbol on the display path");
    expect(has(body, "storedLife + sessionTokens"),
           "INV-2/H-1: visibility gates on stored lifetime + session, not session alone");
    expect(has(body, "this project"),
           "INV-3: the all-time line carries the '(this project' tag");
    expect(has(body, "All projects"), "INV-3: separate 'All projects' line");
    expect(has(body, "tokenUsageReport(false).totalSaved"),
           "INV-4/M-c: 'All projects' reads the GLOBAL getter, not a per-project sum");
    expect(has(body, "setAccessibleName") && has(body, "setAccessibleDescription"),
           "INV-10: accessible name + description set (a11y names the project scope)");
    EXPECT_EQ(0, expect_failures());
}

// INV-4/6/7 — MainWindow: global fold first, per-project fold + single prune +
// single save; the tab-switch refresh calls the pill's named method.
TEST(TokensSavedByProjectWiring, Inv4And6And7MainWindow) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    // Anchor on the DEFINITION signature: `&MainWindow::foldTokenSavingsIntoConfig`
    // appears earlier (the tokenSessionEnding connect), and slurpFunctionBody
    // takes the first match.
    const std::string fold = ants_test::slurpFunctionBody(
        cpp, "void MainWindow::foldTokenSavingsIntoConfig");
    expect(!fold.empty(), "foldTokenSavingsIntoConfig body located");
    const auto globalPos  = fold.find("setClaudeTokensSavedLifetime");
    const auto perProjPos = fold.find("foldProjectBucket");
    expect(globalPos != std::string::npos, "INV-4: global lifetime fold present");
    expect(perProjPos != std::string::npos, "INV-6: per-project foldProjectBucket present");
    if (globalPos != std::string::npos && perProjPos != std::string::npos)
        expect(globalPos < perProjPos, "INV-4: global fold runs BEFORE the per-project fold");
    expect(has(fold, "pruneProjectBuckets"), "INV-6: a single prune pass after the loop");
    expect(count(fold, "m_config.save()") == 1,
           "INV-6: exactly one config.save() covers global + per-project");

    const std::string tab = ants_test::slurpFunctionBody(
        cpp, "void MainWindow::refreshStatusBarForActiveTab");
    expect(!tab.empty(), "refreshStatusBarForActiveTab body located");
    expect(has(tab, "refreshTokensSavedChip"),
           "INV-7: the tab-switch refresh calls refreshTokensSavedChip()");
    EXPECT_EQ(0, expect_failures());
}
