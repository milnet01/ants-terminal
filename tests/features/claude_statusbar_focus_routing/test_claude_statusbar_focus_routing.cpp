// claude status-bar prompt routing + model-chip focus invariants.
// Source-grep harness — see spec.md (ANTS-1835 / ANTS-1840 / ANTS-1849).
// The wiring is GUI-lambda bound (live QStatusBar + PTY-backed terminals +
// a focused window), so we lock the structure rather than drive it live.

#include <cstdio>
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
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

}  // namespace

TEST(ClaudeStatusbarFocusRouting, PermissionPromptGatedOnRouting) {
    const std::string src = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);

    // ANTS-1850/1851 relocated the prompt-UI build out of the slot into the
    // showPermissionPrompt() helper (shared with the tab-switch rebuild).
    // The slot now does only routing + the glyph mark; the helper holds the
    // belongsToFocused gate + message/buttons. The 1835/1840/1849 invariants
    // are unchanged in intent, just split across the two regions.
    const std::string slot = between(
        src, "ClaudeIntegration::permissionRequested",
        "void ClaudeStatusBarController::showPermissionPrompt");
    ASSERT_FALSE(slot.empty())
        << "could not isolate the permissionRequested slot";

    const std::string helper = between(
        src, "void ClaudeStatusBarController::showPermissionPrompt",
        "void ClaudeStatusBarController::maybeShowPromptForActiveTab");
    ASSERT_FALSE(helper.empty())
        << "could not isolate the showPermissionPrompt helper";

    // INV-1 — routing predicate computed in the slot and threaded down.
    EXPECT_NE(slot.find("belongsToFocused"), std::string::npos)
        << "INV-1: slot must compute a belongsToFocused predicate";

    const auto gate = helper.find("if (belongsToFocused)");
    ASSERT_NE(gate, std::string::npos)
        << "INV-1: message/buttons must be guarded by if (belongsToFocused)";

    // INV-1 — the bottom-bar message is emitted only inside the gate.
    const auto msg = helper.find("statusMessageRequested(QString(\"Claude permission");
    ASSERT_NE(msg, std::string::npos)
        << "INV-1: permission status message emit not found";
    EXPECT_GT(msg, gate)
        << "INV-1: statusMessageRequested must be inside the belongsToFocused "
           "branch (a background tab's prompt must not paint on the focused tab)";

    // INV-2 — the glyph marking is unconditional: it lives in the slot and
    // is NOT gated on belongsToFocused, so a background tab's dot still
    // lights even though its message/buttons are suppressed in the helper.
    EXPECT_NE(slot.find("markShellAwaitingInput(awaitingPid, true"), std::string::npos)
        << "INV-2: glyph set-true call not found in the slot";
    EXPECT_EQ(slot.find("if (belongsToFocused)"), std::string::npos)
        << "INV-2: the slot must not gate the glyph mark on belongsToFocused";
}

TEST(ClaudeStatusbarFocusRouting, ModelChipClickHidesAndRefocuses) {
    const std::string src = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);

    // The model-chip click lambda — from the stored-property read to the
    // ANTS-1888 comment that immediately follows the closing }); .
    // Using "});" as the end anchor broke when ANTS-1918 added a nested
    // QTimer lambda whose own "}); " was the first match.
    const std::string clickBody = between(
        src, "m_modelBtn->property(\"modelTier\")", "// ANTS-1888");
    ASSERT_FALSE(clickBody.empty())
        << "could not isolate the model-chip click handler";

    const auto send = clickBody.find("sendToPty");
    ASSERT_NE(send, std::string::npos)
        << "model-chip click must dispatch via sendToPty";

    // INV-3 — chip hidden after dispatch.
    const auto hide = clickBody.find("m_modelBtn->hide()");
    ASSERT_NE(hide, std::string::npos)
        << "INV-3: model-chip click must hide the chip after sending /model";
    EXPECT_GT(hide, send) << "INV-3: hide() must follow sendToPty";

    // INV-4 — focus returned to the terminal after dispatch.
    const auto focus = clickBody.find("focused->setFocus()");
    ASSERT_NE(focus, std::string::npos)
        << "INV-4: model-chip click must return focus to the terminal";
    EXPECT_GT(focus, send) << "INV-4: setFocus() must follow sendToPty";
}
