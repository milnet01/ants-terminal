// Feature-conformance test for ANTS-1888 — per-tab model and thinking-level
// status-bar chips. See tests/features/model_state_chip/spec.md and the
// behavioural coverage of `modelDisplayName` /
// `effortFromLatestAssistantTurn` in tests/features/model_recommender/.
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
    const std::string mw = ants_test::slurpMainWindow();
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

// ANTS-6748 — the chip prints the model's own name and version, not the
// three-value recommender tier, which reads every unknown family as Sonnet.
// The name parser is covered in model_recommender (ModelDisplayName).
TEST(ModelStateChip, ChipNamesTheModelFromItsId) {
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_TRUE(has(cpp, "ModelRecommender::modelDisplayName(rec.currentModel)"))
        << "refreshModelStateChip must name the model with modelDisplayName";
}

// ANTS-6749 — the thinking level has its own pill, fed by what Claude Code
// recorded, and the model pill no longer carries a guessed suffix. The reader
// is covered behaviourally in model_recommender (ModelRecommenderEffort).
TEST(ModelStateChip, ThinkingLevelHasItsOwnPill) {
    const std::string h   = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_H_PATH);
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_TRUE(has(h, "m_thinkingBtn"));
    EXPECT_TRUE(has(cpp, "ModelRecommender::effortFromLatestAssistantTurn(transcriptPath)"))
        << "the pill must show the level the transcript records";
    EXPECT_TRUE(has(cpp, "m_thinkingBtn->setVisible(!effort.isEmpty())"))
        << "no recorded level, no pill: it must never show an empty value";
    EXPECT_TRUE(has(cpp, "if (m_thinkingBtn)   m_thinkingBtn->hide();"))
        << "resetForTabSwitch must hide the pill with the model pill";
    EXPECT_FALSE(has(cpp, "thinkingLevelFromLatestUserTurn"))
        << "the model pill must not append a level guessed from the prompt";
    const auto modelPos = cpp.find("addPermanentWidget(m_modelStateBtn)");
    const auto thinkPos = cpp.find("addPermanentWidget(m_thinkingBtn)");
    ASSERT_NE(thinkPos, std::string::npos);
    EXPECT_LT(modelPos, thinkPos) << "the thinking pill sits beside the model pill";
}
