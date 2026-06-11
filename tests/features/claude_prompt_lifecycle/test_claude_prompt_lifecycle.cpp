// Feature-conformance test for tests/features/claude_prompt_lifecycle/spec.md
// (ANTS-1850 / ANTS-1851).
//
// INV-1..3 + INV-5 lock GUI-lambda wiring that needs a live QStatusBar +
// PTY-backed terminals to drive end-to-end, so they are asserted by
// source-scrape (same approach as claude_statusbar_focus_routing). INV-4
// (tracker rule retention) is exercised behaviourally through the real
// ClaudeTabTracker API.
//
// Exit 0 = all invariants hold.

#include "claudetabtracker.h"
#include "claudeintegration.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {


// Substring of `src` from the first `begin` marker to the first `end`
// marker after it (end exclusive). Empty if either marker is missing.
std::string between(const std::string &src, const std::string &begin,
                    const std::string &end) {
    const auto b = src.find(begin);
    if (b == std::string::npos) return {};
    const auto e = src.find(end, b + begin.size());
    if (e == std::string::npos) return {};
    return src.substr(b, e - b);
}

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

bool write_file(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(body);
    f.close();
    return true;
}

}  // namespace

// INV-1..3, INV-5 — GUI wiring locked by source-scrape.
TEST(ClaudePromptLifecycle, PromptAnchorWiring) {
    const std::string widgets =
        ants_test::slurpFile(ANTS_SOURCE_DIR "/src/claudestatuswidgets.cpp");
    ASSERT_FALSE(widgets.empty());

    const std::string helper = between(
        widgets, "void ClaudeStatusBarController::showPermissionPrompt",
        "void ClaudeStatusBarController::maybeShowPromptForActiveTab");
    ASSERT_FALSE(helper.empty())
        << "could not isolate the showPermissionPrompt helper";

    // INV-1 — per-shell dedup: anchors are deleted only when same-shell or a
    // property-less scroll-scan button; the deleteLater is gated, never bare.
    EXPECT_TRUE(contains(helper, "claudeAwaitingPid"))
        << "INV-1: dedup must read the owning-pid property";
    EXPECT_TRUE(contains(helper, "if (sameShell || scrollScanButton)"))
        << "INV-1: dedup deleteLater must be gated to same-shell / scroll-scan, "
           "not delete every anchor (that orphans other tabs' glyphs)";

    // INV-2 — anchor tagged with its owning shell pid.
    EXPECT_TRUE(contains(helper,
                         "setProperty(\"claudeAwaitingPid\""))
        << "INV-2: anchor must carry its owning-shell pid as a property";

    // INV-3 — retraction scoped to the owning terminal + guarded proxies.
    EXPECT_TRUE(contains(helper, "term->shellPid() == awaitingPid"))
        << "INV-3: claudePermissionCleared must resolve the owning terminal";
    EXPECT_TRUE(contains(helper, "claudePermissionCleared"))
        << "INV-3: primary retraction signal missing";
    EXPECT_TRUE(contains(helper, "focusedMatches"))
        << "INV-3: toolFinished/sessionStopped must guard on focusedMatches so "
           "a background anchor isn't cleared by the focused tab's tool";

    // INV-5 — the rebuild helper gates on retained per-shell state and calls
    // back into showPermissionPrompt with belongsToFocused=true.
    const std::string rebuild = between(
        widgets, "void ClaudeStatusBarController::maybeShowPromptForActiveTab",
        "\n}\n");
    ASSERT_FALSE(rebuild.empty())
        << "could not isolate maybeShowPromptForActiveTab";
    EXPECT_TRUE(contains(rebuild, "awaitingInput") &&
                contains(rebuild, "awaitingRule"))
        << "INV-5: rebuild must gate on the focused shell's awaitingInput + rule";
    EXPECT_TRUE(contains(rebuild, "showPermissionPrompt(focusedPid"))
        << "INV-5: rebuild must re-enter showPermissionPrompt for the focused pid";

    // INV-5 — the tab-switch refresh actually invokes the rebuild.
    const std::string mw = ants_test::slurpFile(ANTS_SOURCE_DIR "/src/mainwindow.cpp");
    ASSERT_FALSE(mw.empty());
    const std::string refresh = between(
        mw, "void MainWindow::refreshStatusBarForActiveTab",
        "void MainWindow::updateStatusBar");
    ASSERT_FALSE(refresh.empty())
        << "could not isolate refreshStatusBarForActiveTab";
    EXPECT_TRUE(contains(refresh, "maybeShowPromptForActiveTab(t->shellPid())"))
        << "INV-5: refreshStatusBarForActiveTab must call the rebuild on switch";
}

// INV-6 (ANTS-1852) — a still-pending BACKGROUND tab's anchor is kept
// alive (hidden) across a switch-away so its retraction wiring can clear
// the dot if the prompt resolves off-tab. Source-scrape (same rationale as
// INV-1..3/5 — driving it end-to-end needs live PTY-backed terminals).
TEST(ClaudePromptLifecycle, BackgroundAnchorSurvivesSwitch) {
    const std::string widgets =
        ants_test::slurpFile(ANTS_SOURCE_DIR "/src/claudestatuswidgets.cpp");
    ASSERT_FALSE(widgets.empty());

    const std::string teardown = between(
        widgets,
        "void ClaudeStatusBarController::clearPromptAnchorsForTabSwitch",
        "void ClaudeStatusBarController::setPromptActive");
    ASSERT_FALSE(teardown.empty())
        << "INV-6: could not isolate clearPromptAnchorsForTabSwitch";

    // The keep-alive decision keys on the owning shell's pid AND the
    // tracker's awaitingInput, scoped to a NON-focused (background) shell.
    EXPECT_TRUE(contains(teardown, "claudeAwaitingPid"))
        << "INV-6: teardown must read the anchor's owning-pid property";
    EXPECT_TRUE(contains(teardown, "shellState") &&
                contains(teardown, "awaitingInput"))
        << "INV-6: keep-alive must gate on the tracker's awaitingInput";
    EXPECT_TRUE(contains(teardown,
                         "p != static_cast<qlonglong>(newlyFocusedPid)"))
        << "INV-6: only BACKGROUND anchors (pid != focused) are kept alive";

    // Pending background anchor is hidden; everything else deleteLater'd.
    EXPECT_TRUE(contains(teardown, "w->hide()"))
        << "INV-6: a still-pending background anchor must be hidden, not deleted";
    EXPECT_TRUE(contains(teardown, "w->deleteLater()"))
        << "INV-6: focused / scroll-scan / resolved anchors must still delete";

    // Wired into the tab-switch teardown, fed the tab being switched TO.
    const std::string mw = ants_test::slurpFile(ANTS_SOURCE_DIR "/src/mainwindow.cpp");
    ASSERT_FALSE(mw.empty());
    const std::string refresh = between(
        mw, "void MainWindow::refreshStatusBarForActiveTab",
        "void MainWindow::updateStatusBar");
    ASSERT_FALSE(refresh.empty());
    EXPECT_TRUE(contains(refresh, "clearPromptAnchorsForTabSwitch("))
        << "INV-6: Category-C teardown must route through the controller helper";
    // Reverting INV-6 means restoring the blanket loop — guard against it.
    EXPECT_FALSE(contains(refresh, "for (QWidget *w : staleAllowBtns)"))
        << "INV-6: the blanket findChildren->deleteLater loop must be replaced";
}

// INV-4 — tracker rule retention round-trip, underlying state preserved.
TEST(ClaudePromptLifecycle, RuleRetention) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + "/s.jsonl";
    ASSERT_TRUE(write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{}}]}}
)"));

    ClaudeTabTracker tracker;
    const pid_t kPid = 4242;
    tracker.forceRefreshForTest(kPid, path);
    ASSERT_EQ(tracker.shellState(kPid).state, ClaudeState::ToolUse);

    QSignalSpy spy(&tracker, &ClaudeTabTracker::shellStateChanged);

    // Set awaiting with a rule — flag + rule stored, underlying state kept.
    tracker.markShellAwaitingInput(kPid, true, QStringLiteral("Bash(ls -la)"));
    auto s1 = tracker.shellState(kPid);
    EXPECT_TRUE(s1.awaitingInput);
    EXPECT_EQ(s1.awaitingRule, QStringLiteral("Bash(ls -la)"));
    EXPECT_EQ(s1.state, ClaudeState::ToolUse)
        << "underlying state must survive the awaiting overlay";
    EXPECT_EQ(spy.count(), 1);

    // Re-prompt while already awaiting refreshes the rule WITHOUT re-firing
    // (glyph state unchanged — maybeEmit ignores awaitingRule).
    tracker.markShellAwaitingInput(kPid, true, QStringLiteral("Bash(rm x)"));
    auto s2 = tracker.shellState(kPid);
    EXPECT_EQ(s2.awaitingRule, QStringLiteral("Bash(rm x)"))
        << "a re-prompt must update the retained rule";
    EXPECT_EQ(spy.count(), 1) << "rule-only change must not re-emit";

    // Clearing drops both the flag and the rule; underlying state kept.
    tracker.markShellAwaitingInput(kPid, false);
    auto s3 = tracker.shellState(kPid);
    EXPECT_FALSE(s3.awaitingInput);
    EXPECT_TRUE(s3.awaitingRule.isEmpty())
        << "clearing awaitingInput must clear the retained rule";
    EXPECT_EQ(s3.state, ClaudeState::ToolUse);
    EXPECT_EQ(spy.count(), 2);
}
