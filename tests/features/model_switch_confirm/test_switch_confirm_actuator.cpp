// ANTS-1920 — output-driven model-switch confirm: actuator-wiring guard.
// Source-grep over src/claudestatuswidgets.cpp (the handshake/poll) and
// src/modelautoswitch.h (the detector declaration). The pure detector is
// exercised by test_switch_confirm_visible.cpp. See spec.md INV-5/INV-6.

#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif
#ifndef SRC_MODELAUTOSWITCH_H_PATH
#error "SRC_MODELAUTOSWITCH_H_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The handshake + poll region: from performModelSwitchHandshake's definition
// up to the next function (maybeFireDeferredChipSwitch). Excludes the prose
// doc-comment above the signature.
std::string handshakeRegion(const std::string &src) {
    const auto b = src.find(
        "void ClaudeStatusBarController::performModelSwitchHandshake");
    const auto e = src.find(
        "void ClaudeStatusBarController::maybeFireDeferredChipSwitch");
    if (b == std::string::npos || e == std::string::npos || e <= b) return {};
    return src.substr(b, e - b);
}

}  // namespace

// INV-5 — the confirm is output-driven: the region polls
// switchConfirmVisible, and the confirm CR is sent only after that check.
TEST(SwitchConfirmActuator, Inv5ConfirmIsOutputGated) {
    const std::string region = handshakeRegion(slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH));
    ASSERT_FALSE(region.empty()) << "handshake region not found";

    const auto guardPos = region.find("switchConfirmVisible");
    EXPECT_NE(guardPos, std::string::npos)
        << "INV-5: confirm must be gated on switchConfirmVisible";

    const auto confirmPos = region.find("sendToPty(\"\\r\")");
    EXPECT_NE(confirmPos, std::string::npos)
        << "INV-5: confirm CR send not found";
    EXPECT_LT(guardPos, confirmPos)
        << "INV-5: the dialog check must precede the confirm CR";
}

// INV-5 — the blind 400 ms confirm timer is gone (no QTimer::singleShot(400).
TEST(SwitchConfirmActuator, Inv5NoBlindConfirmTimer) {
    const std::string region = handshakeRegion(slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH));
    ASSERT_FALSE(region.empty());
    EXPECT_EQ(region.find("singleShot(400"), std::string::npos)
        << "INV-5: the blind 400 ms confirm timer must not return";
}

// INV-5 — budget exhaustion aborts with ESC, never a blind CR.
TEST(SwitchConfirmActuator, Inv5AbortSendsEsc) {
    const std::string region = handshakeRegion(slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH));
    ASSERT_FALSE(region.empty());
    EXPECT_NE(region.find("\\x1b"), std::string::npos)
        << "INV-5: abort branch must send ESC to clear stranded /model";
}

// INV-6 — the poll is bounded by kSwitchConfirmMaxPolls.
TEST(SwitchConfirmActuator, Inv6BoundedPoll) {
    const std::string region = handshakeRegion(slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH));
    ASSERT_FALSE(region.empty());
    EXPECT_NE(region.find("kSwitchConfirmMaxPolls"), std::string::npos)
        << "INV-6: the poll must be bounded by kSwitchConfirmMaxPolls";
}

// The detector is declared in modelautoswitch.h (pure, in ants_claude_lib).
TEST(SwitchConfirmActuator, DetectorDeclared) {
    const std::string hdr = slurp(SRC_MODELAUTOSWITCH_H_PATH);
    EXPECT_NE(hdr.find("bool switchConfirmVisible("), std::string::npos)
        << "switchConfirmVisible must be declared in modelautoswitch.h";
}
