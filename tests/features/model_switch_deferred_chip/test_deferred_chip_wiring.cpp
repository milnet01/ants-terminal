// ANTS-1915 — deferred manual chip-switch wiring guard. Source-grep against
// the controller (mirrors the ANTS-1735 INV-14 actuator-wiring guard): the
// live click → defer → state-change → fire path needs a TerminalWidget +
// tracker harness that doesn't exist, so we assert the wiring is present and
// correctly ordered. See tests/features/model_switch_deferred_chip/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif

namespace {


// Brace-balanced body of a function starting at `marker`.
std::string functionBody(const std::string &src, const std::string &marker) {
    const auto h = src.find(marker);
    if (h == std::string::npos) return {};
    const auto open = src.find('{', h);
    if (open == std::string::npos) return {};
    int depth = 1; size_t i = open + 1;
    for (; i < src.size() && depth > 0; ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
    }
    return src.substr(open, i - open);
}

}  // namespace

// INV-1 — the chip-click handler defers when the focused shell is generating,
// recording the tier + shell pid and returning before the immediate sendToPty.
TEST(DeferredChipSwitch, Inv1ClickDefersWhileGenerating) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    // The deferral branch must reference the generating states and the
    // deferral state, and it must precede the immediate /model send.
    const auto deferPos = src.find("m_deferredChipTier     = tier;");
    ASSERT_NE(deferPos, std::string::npos)
        << "chip-click handler must set m_deferredChipTier when generating";
    // The generating predicate covers the three non-idle active states.
    EXPECT_NE(src.find("ClaudeState::Thinking"), std::string::npos);
    EXPECT_NE(src.find("ClaudeState::ToolUse"), std::string::npos);
    EXPECT_NE(src.find("ClaudeState::Compacting"), std::string::npos);
    // The immediate send (the non-deferred path) must come AFTER the defer
    // branch's early return — i.e. the deferral guards the send.
    const auto sendPos = src.find(
        "(QStringLiteral(\"/model \") + tier + QStringLiteral(\"\\r\")).toUtf8());");
    ASSERT_NE(sendPos, std::string::npos);
    EXPECT_LT(deferPos, sendPos)
        << "the defer branch must precede (guard) the immediate /model send";
}

// INV-2 — maybeFireDeferredChipSwitch is wired to shellStateChanged.
TEST(DeferredChipSwitch, Inv2FireWiredToStateChange) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    // A connect to shellStateChanged whose lambda calls the fire helper.
    EXPECT_NE(src.find("maybeFireDeferredChipSwitch(shellPid);"),
              std::string::npos)
        << "shellStateChanged must drive maybeFireDeferredChipSwitch";
    EXPECT_NE(src.find("ClaudeTabTracker::shellStateChanged"),
              std::string::npos);
}

// INV-3/INV-4/INV-5 — the fire helper gates on Idle, resolves the terminal by
// pid, clears the deferral, and seeds the override cool-down.
TEST(DeferredChipSwitch, Inv345FireHelperContract) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body = functionBody(
        src, "void ClaudeStatusBarController::maybeFireDeferredChipSwitch");
    ASSERT_FALSE(body.empty()) << "maybeFireDeferredChipSwitch body not found";
    // INV-3: only fires when the shell is Idle.
    EXPECT_NE(body.find("ClaudeState::Idle"), std::string::npos)
        << "INV-3: must gate on Idle";
    // INV-3: pid must match the pending deferral.
    EXPECT_NE(body.find("shellPid != m_deferredChipShellPid"), std::string::npos)
        << "INV-3: must match the pending deferral pid";
    // INV-4: terminal resolved by shellPid via the tab provider.
    EXPECT_NE(body.find("m_terminalAtTabProvider"), std::string::npos)
        << "INV-4: terminal resolved by pid (not focus)";
    // INV-4: deferral state cleared.
    EXPECT_NE(body.find("m_deferredChipTier.clear();"), std::string::npos)
        << "INV-4: deferral cleared on fire";
    // INV-4: override cool-down seeded so the auto-switcher doesn't undo it.
    EXPECT_NE(body.find("m_lastOverrideMsByProject["), std::string::npos)
        << "INV-4: must seed the ANTS-1890 override cool-down";
    // INV-4: the switch goes through the shared handshake.
    EXPECT_NE(body.find("performModelSwitchHandshake(term)"), std::string::npos)
        << "INV-4: must use the shared ESC+ENTER handshake";
    // INV-5: a missing terminal drops the deferral without sending.
    const auto noTermPos = body.find("if (!term)");
    ASSERT_NE(noTermPos, std::string::npos) << "INV-5: must handle tab-closed";
    const auto sendPos = body.find("term->sendToPty(");
    ASSERT_NE(sendPos, std::string::npos);
    EXPECT_LT(noTermPos, sendPos)
        << "INV-5: the tab-closed drop must precede the send";
}
