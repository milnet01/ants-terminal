// Feature-conformance test for tests/features/claude_question_prompt_dot/spec.md
// (ANTS-1858).
//
// INV-1..4 lock scanner + GUI-lambda wiring that needs a PTY-backed grid +
// a live QStatusBar, so they are asserted by source-scrape (same approach
// as claude_prompt_lifecycle). INV-5 (tracker awaiting overlay) is
// exercised behaviourally through the real ClaudeTabTracker API.
//
// Exit 0 = all invariants hold.

#include "claudetabtracker.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

std::string slurp(const std::string &path) {
    std::ifstream f(path);
    if (!f) { ADD_FAILURE() << "cannot open " << path; return {}; }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Substring from the first `begin` marker to the first `end` after it.
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

}  // namespace

// INV-1..3 — scanner branch: footer anchor, permission exclusion, signals,
// debounce.
TEST(ClaudeQuestionPromptDot, ScannerDetectsSelectionFooter) {
    const std::string tw = slurp(ANTS_SOURCE_DIR "/src/terminalwidget.cpp");
    const std::string scan = between(tw,
        "void TerminalWidget::checkForClaudePermissionPrompt()",
        "// Claude Code permission prompts have this structure");
    ASSERT_FALSE(scan.empty()) << "question branch region not found";

    EXPECT_TRUE(contains(scan, "Enter to select"))
        << "INV-1: anchors on the ASCII-stable selection footer";
    EXPECT_TRUE(contains(scan, "emit claudeQuestionDetected()"))
        << "INV-1: emits the rule-less question-detected signal";
    EXPECT_TRUE(contains(scan, "emit claudeQuestionCleared()"))
        << "INV-3: emits the cleared signal on the debounce path";
    EXPECT_TRUE(contains(scan, "questionFooter && !permFooter"))
        << "INV-2: permission prompt wins — question fires only when no "
           "permission footer is present";
    EXPECT_TRUE(contains(scan, "m_claudeQuestionMissedCount >= 3"))
        << "INV-3: N=3 consecutive-miss debounce before clearing";
}

// INV-4 — mainwindow handler lights dot + label, creates NO button.
TEST(ClaudeQuestionPromptDot, MainwindowWiringNoButton) {
    const std::string mw = slurp(ANTS_SOURCE_DIR "/src/mainwindow.cpp");

    const std::string detected = between(mw,
        "&TerminalWidget::claudeQuestionDetected",
        "&TerminalWidget::claudeQuestionCleared");
    ASSERT_FALSE(detected.empty())
        << "claudeQuestionDetected connect region not found";
    EXPECT_TRUE(contains(detected, "markShellAwaitingInput(pid, true)"))
        << "INV-4: detected handler lights the awaiting-input dot";
    EXPECT_TRUE(contains(detected, "setPromptActive(true)"))
        << "INV-4: detected handler sets the 'Claude: prompting' label";
    EXPECT_FALSE(contains(detected, "claudeAllowBtn"))
        << "INV-4: a question has no allowlist rule — no Add-to-allowlist "
           "button";
    EXPECT_FALSE(contains(detected, "Add to allowlist"))
        << "INV-4: no allowlist button in the question path";

    const std::string cleared = between(mw,
        "&TerminalWidget::claudeQuestionCleared",
        "void MainWindow::newTab()");
    ASSERT_FALSE(cleared.empty())
        << "claudeQuestionCleared connect region not found";
    EXPECT_TRUE(contains(cleared, "markShellAwaitingInput(pid, false)"))
        << "INV-4: cleared handler drops the awaiting-input dot";
}

// INV-5 — tracker awaiting overlay (no rule) drives the orange glyph.
TEST(ClaudeQuestionPromptDot, AwaitingOverlayNoRule) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + "/s.jsonl";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"AskUserQuestion","input":{}}]}}
)");
    f.close();

    ClaudeTabTracker tracker;
    const pid_t kPid = 5151;
    tracker.forceRefreshForTest(kPid, path);

    // A question carries no allowlist rule — the 2-arg overlay.
    tracker.markShellAwaitingInput(kPid, true);
    auto s1 = tracker.shellState(kPid);
    EXPECT_TRUE(s1.awaitingInput)
        << "INV-5: awaitingInput overlay set (glyph provider → orange)";
    EXPECT_TRUE(s1.awaitingRule.isEmpty())
        << "INV-5: a question sets no awaitingRule";

    tracker.markShellAwaitingInput(kPid, false);
    EXPECT_FALSE(tracker.shellState(kPid).awaitingInput)
        << "INV-5: answering clears the overlay";
}
