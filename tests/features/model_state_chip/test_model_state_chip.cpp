// Feature-conformance test for ANTS-1888 — per-tab model + thinking-level
// status-bar chip. See tests/features/model_state_chip/spec.md and the
// behavioural coverage of `thinkingLevelFromLatestUserTurn` /
// `thinkingLevelLabel` in tests/features/model_recommender/.
//
// This bundle is a source-grep contract: the chip is a GUI widget whose
// wiring (member, refresh method, 2 s timer connect, tab-switch reset)
// must stay locked even as the controller evolves. The behavioural parser
// tests live in the model_recommender bundle to avoid duplicating
// JSONL-fixture plumbing.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// The chip's QPushButton member lives on the controller; the refresh
// method is declared public so the status-timer slot can find it.
TEST(ModelStateChip, ControllerDeclaresWidgetAndRefresh) {
    const std::string h   = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_H_PATH);
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);

    EXPECT_TRUE(has(h, "m_modelStateBtn"))
        << "claudestatuswidgets.h must declare m_modelStateBtn (ANTS-1888)";
    EXPECT_TRUE(has(h, "refreshModelStateChip"))
        << "claudestatuswidgets.h must declare refreshModelStateChip() (ANTS-1888)";
    EXPECT_TRUE(has(cpp, "void ClaudeStatusBarController::refreshModelStateChip"))
        << "claudestatuswidgets.cpp must define refreshModelStateChip (ANTS-1888)";
    // The chip must be wired to addPermanentWidget AFTER m_modelBtn so it
    // occupies the recommender's slot when INV-14 hides it.
    const auto modelBtnPos       = cpp.find("addPermanentWidget(m_modelBtn)");
    const auto modelStateBtnPos  = cpp.find("addPermanentWidget(m_modelStateBtn)");
    ASSERT_NE(modelBtnPos,      std::string::npos)
        << "m_modelBtn must be added via addPermanentWidget";
    ASSERT_NE(modelStateBtnPos, std::string::npos)
        << "m_modelStateBtn must be added via addPermanentWidget";
    EXPECT_LT(modelBtnPos, modelStateBtnPos)
        << "m_modelStateBtn must be added AFTER m_modelBtn (ANTS-1888)";
}

// The 2 s status timer must call the chip's refresh; otherwise the chip
// only updates on tab-switch.
TEST(ModelStateChip, StatusTimerConnectsToRefresh) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP);
    EXPECT_TRUE(has(mw, "refreshModelStateChip"))
        << "mainwindow.cpp must reference refreshModelStateChip (ANTS-1888)";
    EXPECT_TRUE(has(mw, "ANTS-1888"))
        << "the status-timer connect must anchor ANTS-1888";
}

// resetForTabSwitch must hide the chip and clear its mtime cache so the
// next tick re-paints against the newly focused tab — the same pattern
// the recommender chip uses.
TEST(ModelStateChip, ResetForTabSwitchHidesChipAndClearsMtime) {
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_TRUE(has(cpp, "m_modelStateBtn->hide()"))
        << "resetForTabSwitch must hide m_modelStateBtn (ANTS-1888)";
    EXPECT_TRUE(has(cpp, "m_modelStatePath.clear()"))
        << "resetForTabSwitch must clear m_modelStatePath (ANTS-1888)";
    EXPECT_TRUE(has(cpp, "m_modelStateMtimeMs = -1"))
        << "resetForTabSwitch must reset m_modelStateMtimeMs (ANTS-1888)";
}

// The chip must NEVER render the word "Unknown" — when the parser returns
// Unknown, thinkingLevelLabel returns empty and the chip drops the " · …"
// suffix. The render path checks isEmpty() before appending.
TEST(ModelStateChip, RenderPathGuardsAgainstUnknownLabel) {
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_TRUE(has(cpp, "thinkLabel.isEmpty()"))
        << "refreshModelStateChip must guard the \" · \" suffix on "
           "an empty thinkLabel so \"Unknown\" never reaches the UI";
}
