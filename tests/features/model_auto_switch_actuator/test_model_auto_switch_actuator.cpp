// ANTS-1735 INV-14 actuator-wiring guard. Source-grep against the
// controller and MainWindow timer-wire site. The pure decide() / ledger
// / stats verb are exercised by their own test bundles.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {


std::string between(const std::string &src, const std::string &begin,
                    const std::string &end) {
    const auto b = src.find(begin);
    if (b == std::string::npos) return {};
    const auto e = src.find(end, b + begin.size());
    if (e == std::string::npos) return {};
    return src.substr(b, e - b);
}

}  // namespace

// INV-14 (a) — refreshAutoModelSwitch reads claudeAutoModel() and
// returns immediately when switch_enabled is false (default-off).
TEST(ModelAutoSwitchActuator, Inv14BailsWhenDisabled) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body = between(
        src, "void ClaudeStatusBarController::refreshAutoModelSwitch()",
        "qint64 ClaudeStatusBarController::kMaxDwellSentinel");
    ASSERT_FALSE(body.empty())
        << "INV-14(a): refreshAutoModelSwitch body not found";
    EXPECT_NE(body.find("claudeAutoModel()"), std::string::npos)
        << "INV-14(a): config must be read via claudeAutoModel()";
    EXPECT_NE(body.find("switch_enabled"), std::string::npos)
        << "INV-14(a): must gate on switch_enabled";
    // The bail must precede every other read — the "if (!enabled)
    // return;" pattern lives near the top.
    const auto enabledPos = body.find("switch_enabled");
    const auto firstReturn = body.find("return;");
    ASSERT_NE(firstReturn, std::string::npos);
    EXPECT_LT(firstReturn, body.find("ModelAutoSwitch::decide("))
        << "INV-14(a): early-return must precede decide() to avoid "
           "doing work when disabled";
    (void)enabledPos;
}

// INV-14 (b) — refreshModelChip suppresses the Shape A chip when the
// autonomous switcher is enabled.
TEST(ModelAutoSwitchActuator, Inv14ChipSuppressedWhenEnabled) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body = between(
        src, "void ClaudeStatusBarController::refreshModelChip()",
        "void ClaudeStatusBarController::refreshAutoModelSwitch");
    ASSERT_FALSE(body.empty())
        << "INV-14(b): refreshModelChip body not found";
    EXPECT_NE(body.find("claudeAutoModel()"), std::string::npos)
        << "INV-14(b): chip suppression must read claudeAutoModel()";
    EXPECT_NE(body.find("switch_enabled"), std::string::npos)
        << "INV-14(b): suppression gate must check switch_enabled";
    // The hide()+return must happen before the transcript-path /
    // score work — same shape as the existing transcriptPath empty
    // early return.
    const auto switchPos = body.find("switch_enabled");
    const auto scorePos = body.find("ModelRecommender::score(");
    ASSERT_NE(scorePos, std::string::npos);
    EXPECT_LT(switchPos, scorePos)
        << "INV-14(b): suppression gate must precede ModelRecommender::score";
}

// INV-14 (c) — the 2 s status timer drives the actuator.
TEST(ModelAutoSwitchActuator, Inv14WiredOnStatusTimer) {
    const std::string src = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    // Look for the connect line tying m_statusTimer to
    // refreshAutoModelSwitch — same shape MainWindow uses for the
    // other status-timer slots.
    EXPECT_NE(src.find("refreshAutoModelSwitch"), std::string::npos)
        << "INV-14(c): refreshAutoModelSwitch must be referenced in "
           "MainWindow (timer wire site)";
    // Must be near m_statusTimer in the same connect block.
    const auto wirePos = src.find("refreshAutoModelSwitch");
    ASSERT_NE(wirePos, std::string::npos);
    // Scan 600 chars backwards for m_statusTimer.
    const size_t start = wirePos > 600 ? wirePos - 600 : 0;
    const std::string window = src.substr(start, wirePos - start);
    EXPECT_NE(window.find("m_statusTimer"), std::string::npos)
        << "INV-14(c): refreshAutoModelSwitch connect must reference "
           "m_statusTimer (the 2 s tick)";
}
