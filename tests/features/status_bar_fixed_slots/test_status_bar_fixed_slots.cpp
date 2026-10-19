// ANTS-5753 — fixed status-bar slots. See spec.md.

#include "claudestatuswidgets.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QLabel>
#include <QPushButton>
#include <QStringList>

#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDESTATUSWIDGETS_H_PATH
#error "SRC_CLAUDESTATUSWIDGETS_H_PATH compile definition required"
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

const QStringList kCandidates{QStringLiteral("a"),
                              QStringLiteral("a much longer candidate text")};

template <typename W>
void expectPinned(W &w) {
    w.setText(QStringLiteral("a"));
    statusbarslot::pin(&w, kCandidates);

    EXPECT_EQ(w.text().toStdString(), std::string("a"))
        << "pin must restore the widget's own text";
    EXPECT_EQ(w.minimumWidth(), w.maximumWidth()) << "width must be fixed";
    EXPECT_TRUE(w.sizePolicy().retainSizeWhenHidden());

    const int pinned = w.maximumWidth();
    w.setText(kCandidates.last());
    EXPECT_GE(pinned, w.sizeHint().width())
        << "the slot must fit the widest candidate";
    w.setText(QStringLiteral("a"));
    EXPECT_EQ(w.maximumWidth(), pinned)
        << "the width must not follow the current text";
}

}  // namespace

// INV-1 — the helper on a label and a button.
TEST(StatusBarFixedSlots, Inv1PinFitsWidestAndKeepsText) {
    QLabel label;
    expectPinned(label);
    QPushButton button;
    expectPinned(button);
}

// INV-2 — a switched-off pill gives its slot back.
TEST(StatusBarFixedSlots, Inv2SetRetainFalseGivesSlotBack) {
    QLabel label;
    statusbarslot::pin(&label, kCandidates);
    ASSERT_TRUE(label.sizePolicy().retainSizeWhenHidden());
    statusbarslot::setRetain(&label, false);
    EXPECT_FALSE(label.sizePolicy().retainSizeWhenHidden());
}

// INV-3 — every variable-text pill is pinned, and a theme change re-pins.
TEST(StatusBarFixedSlots, Inv3EveryPillIsPinned) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body = between(
        src, "void ClaudeStatusBarController::pinSlots()", "\n}\n");
    ASSERT_FALSE(body.empty()) << "pinSlots() not found";
    for (const char *m : {"m_statusLabel", "m_tokensSavedChip", "m_mailChip",
                          "m_reviewBtn", "m_bgTasksBtn", "m_tasksBtn",
                          "m_modelBtn", "m_undoSwitchBtn", "m_errorLabel"}) {
        EXPECT_NE(body.find(std::string("statusbarslot::pin(") + m),
                  std::string::npos)
            << m << " must be pinned in pinSlots()";
    }
    EXPECT_NE(body.find("statusbarslot::setRetain(m_contextBar"),
              std::string::npos)
        << "the context meter's slot must survive hide()";

    const std::string theme = between(
        src, "void ClaudeStatusBarController::applyTheme(", "\n}\n");
    ASSERT_FALSE(theme.empty());
    EXPECT_NE(theme.find("pinSlots();"), std::string::npos)
        << "applyTheme must re-measure the slots";

    const std::string hdr = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_H_PATH);
    EXPECT_NE(hdr.find("void pinSlots();"), std::string::npos);
}

// INV-4 — a state text wider than its slot is elided, full text in tooltip.
TEST(StatusBarFixedSlots, Inv4StateTextIsElided) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body = between(
        src, "void ClaudeStatusBarController::apply()",
        "void ClaudeStatusBarController::");
    ASSERT_FALSE(body.empty()) << "apply() not found";
    const auto elide = body.find("elidedText(");
    const auto set = body.find("m_statusLabel->setText(");
    ASSERT_NE(elide, std::string::npos) << "apply() must elide the state text";
    ASSERT_NE(set, std::string::npos);
    EXPECT_LT(elide, set) << "elide before setText";
    EXPECT_NE(body.find("m_statusLabel->setToolTip("), std::string::npos)
        << "the full text must reach the tooltip";
}

// INV-5 — the Roadmap button keeps its slot; the allowlist prompt goes left.
TEST(StatusBarFixedSlots, Inv5RoadmapRetainedAllowlistInsertedLeft) {
    const std::string mw = ants_test::slurpMainWindow();
    EXPECT_NE(mw.find("statusbarslot::setRetain(m_roadmapBtn"),
              std::string::npos)
        << "the Roadmap button must keep its slot when hidden";
    EXPECT_NE(mw.find("insertPermanentWidget(0, addBtn)"), std::string::npos)
        << "scroll-scan allowlist prompt must insert at the left end";
    EXPECT_EQ(mw.find("addPermanentWidget(addBtn)"), std::string::npos);

    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_NE(src.find("insertPermanentWidget(0, btnWidget)"), std::string::npos)
        << "hook allowlist prompt must insert at the left end";
    EXPECT_EQ(src.find("addPermanentWidget(btnWidget)"), std::string::npos);
}

// INV-6 (ANTS-5762) — the occasional pills sit at the left and hold no slot;
// the steady pills follow in the user's order; the model pill fits its text.
TEST(StatusBarFixedSlots, Inv6OccasionalLeftSteadyInOrder) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    std::size_t at = 0;
    for (const char *m : {"m_mailChip", "m_bgTasksBtn", "m_tasksBtn",
                          "m_undoSwitchBtn", "m_errorLabel", "m_statusLabel",
                          "m_modelBtn", "m_modelStateBtn", "m_contextBar",
                          "m_tokensSavedChip", "m_reviewBtn"}) {
        const std::string call = std::string("addPermanentWidget(") + m + ")";
        const auto pos = src.find(call);
        ASSERT_NE(pos, std::string::npos) << call;
        EXPECT_GT(pos, at) << m << " is out of order";
        at = pos;
    }

    const std::string body = between(
        src, "void ClaudeStatusBarController::pinSlots()", "\n}\n");
    ASSERT_FALSE(body.empty());
    for (const char *m : {"m_mailChip", "m_bgTasksBtn", "m_tasksBtn",
                          "m_errorLabel"}) {
        const std::string stmt = between(
            body, std::string("statusbarslot::pin(") + m, ";");
        EXPECT_NE(stmt.find("/*retainWhenHidden=*/false"), std::string::npos)
            << m << " must give its slot back when hidden";
    }
    const std::string undo = between(
        body, "statusbarslot::pin(m_undoSwitchBtn", ";");
    EXPECT_NE(undo.find("/*retainWhenHidden=*/false"), std::string::npos)
        << "the Undo pill must give its slot back when hidden";
    EXPECT_EQ(body.find("statusbarslot::pin(m_modelStateBtn"), std::string::npos)
        << "the model pill fits its text";
    EXPECT_NE(body.find("statusbarslot::setRetain(m_modelStateBtn, true)"),
              std::string::npos);
}
