// Feature-conformance test for tests/features/claude_state_resolver/spec.md
// (ANTS-1873).
//
// Behavioural tests of the pure helper `claudestate::*`, plus source-
// grep that both surfaces (status-bar apply + tab-dot lambda) consume
// it. The third leg (INV-11, equivalence to the prior cached-scalar
// ladder) is locked by the existing claude-status test suites remaining
// green.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <cctype>
#include <string>

#include "claudestateresolver.h"
#include "claudetabtracker.h"
#include "claudeintegration.h"   // ClaudeState

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif

using claudestate::Display;
using claudestate::Resolved;

namespace {


ClaudeTabTracker::ShellState makeShell(ClaudeState s,
                                       const QString &tool = QString(),
                                       bool planMode = false,
                                       bool auditing = false,
                                       bool awaitingInput = false) {
    ClaudeTabTracker::ShellState st;
    st.state = s;
    st.tool = tool;
    st.planMode = planMode;
    st.auditing = auditing;
    st.awaitingInput = awaitingInput;
    return st;
}

}  // namespace

// INV-1 — fromShell is a lossless adapter.
TEST(ClaudeStateResolver, Inv1FromShellLossless) {
    const auto st = makeShell(ClaudeState::Thinking,
                              QStringLiteral("Read"),
                              /*planMode=*/true,
                              /*auditing=*/false,
                              /*awaitingInput=*/true);
    const Resolved r = claudestate::fromShell(st);
    EXPECT_EQ(r.base, ClaudeState::Thinking);
    EXPECT_EQ(r.tool, QStringLiteral("Read"));
    EXPECT_TRUE(r.planMode);
    EXPECT_FALSE(r.auditing);
    EXPECT_TRUE(r.awaitingInput);

    // ANTS-5773 — the adapter also carries the background counts and
    // their sum.
    auto bg = makeShell(ClaudeState::Idle);
    bg.backgroundShells = 1;
    bg.backgroundAgents = 2;
    bg.backgroundMonitors = 4;
    const Resolved rb = claudestate::fromShell(bg);
    EXPECT_EQ(rb.backgroundShells, 1);
    EXPECT_EQ(rb.backgroundAgents, 2);
    EXPECT_EQ(rb.backgroundMonitors, 4);
    EXPECT_EQ(rb.backgroundPending, 7);
}

// INV-2 — awaitingInput wins over every other field.
TEST(ClaudeStateResolver, Inv2AwaitingInputBeatsEverything) {
    Resolved r;
    r.awaitingInput = true;

    for (auto base : {ClaudeState::NotRunning, ClaudeState::Idle,
                      ClaudeState::Thinking, ClaudeState::ToolUse,
                      ClaudeState::Compacting}) {
        r.base = base;
        for (bool pm : {false, true}) {
            for (bool au : {false, true}) {
                r.planMode = pm;
                r.auditing = au;
                EXPECT_EQ(claudestate::display(r), Display::AwaitingInput)
                    << "awaitingInput must win for base=" << int(base)
                    << " planMode=" << pm << " auditing=" << au;
            }
        }
    }
}

// INV-3 — Planning requires base != NotRunning; collapses to Hidden
// otherwise.
TEST(ClaudeStateResolver, Inv3PlanningRequiresRunning) {
    Resolved r;
    r.planMode = true;

    r.base = ClaudeState::Idle;
    EXPECT_EQ(claudestate::display(r), Display::Planning);
    r.base = ClaudeState::Thinking;
    EXPECT_EQ(claudestate::display(r), Display::Planning);
    r.base = ClaudeState::ToolUse;
    EXPECT_EQ(claudestate::display(r), Display::Planning);
    r.base = ClaudeState::Compacting;
    EXPECT_EQ(claudestate::display(r), Display::Planning);

    r.base = ClaudeState::NotRunning;
    EXPECT_EQ(claudestate::display(r), Display::Hidden);
}

// INV-4 — Auditing requires base != NotRunning + no higher overlay.
TEST(ClaudeStateResolver, Inv4AuditingRequiresRunning) {
    Resolved r;
    r.auditing = true;

    r.base = ClaudeState::Thinking;
    EXPECT_EQ(claudestate::display(r), Display::Auditing);

    r.base = ClaudeState::NotRunning;
    EXPECT_EQ(claudestate::display(r), Display::Hidden);

    // planMode beats auditing.
    r.base = ClaudeState::Thinking;
    r.planMode = true;
    EXPECT_EQ(claudestate::display(r), Display::Planning);
}

// INV-5 — Bash splits off; case-insensitive match.
TEST(ClaudeStateResolver, Inv5ToolUseBashSplit) {
    Resolved r;
    r.base = ClaudeState::ToolUse;

    r.tool = QStringLiteral("Bash");
    EXPECT_EQ(claudestate::display(r), Display::ToolUseBash);
    r.tool = QStringLiteral("bash");
    EXPECT_EQ(claudestate::display(r), Display::ToolUseBash);
    r.tool = QStringLiteral("BASH");
    EXPECT_EQ(claudestate::display(r), Display::ToolUseBash);

    r.tool = QStringLiteral("Read");
    EXPECT_EQ(claudestate::display(r), Display::ToolUseGeneric);
    r.tool = QStringLiteral("WebFetch");
    EXPECT_EQ(claudestate::display(r), Display::ToolUseGeneric);
    r.tool.clear();
    EXPECT_EQ(claudestate::display(r), Display::ToolUseGeneric);
}

// ANTS-5718 — a tool call that blocks on the user's choice is
// AwaitingInput in every tab, not ToolUse: the screen scan that lights
// a question (ANTS-1858) only sees the focused tab. Plan approval
// waits too, so it beats Planning.
TEST(ClaudeStateResolver, Inv5bChoiceToolsAwaitInput) {
    Resolved r;
    r.base = ClaudeState::ToolUse;
    r.tool = QStringLiteral("AskUserQuestion");
    EXPECT_EQ(claudestate::display(r), Display::AwaitingInput);
    r.tool = QStringLiteral("ExitPlanMode");
    r.planMode = true;
    EXPECT_EQ(claudestate::display(r), Display::AwaitingInput);
    // Only while the call is in flight: the same name on another state
    // is a finished call.
    r.base = ClaudeState::Thinking;
    r.planMode = false;
    EXPECT_EQ(claudestate::display(r), Display::Thinking);
}

// INV-6 — NotRunning collapses to Hidden; awaitingInput is the one
// signal that can fire BEFORE any transcript event lands.
TEST(ClaudeStateResolver, Inv6NotRunningHidden) {
    Resolved r;
    r.base = ClaudeState::NotRunning;
    EXPECT_EQ(claudestate::display(r), Display::Hidden);

    r.awaitingInput = true;
    EXPECT_EQ(claudestate::display(r), Display::AwaitingInput);
}

// INV-6b — All base states map straightforwardly absent overlays.
TEST(ClaudeStateResolver, Inv6BaseStatesMap) {
    Resolved r;
    r.base = ClaudeState::Idle;
    EXPECT_EQ(claudestate::display(r), Display::Idle);
    r.base = ClaudeState::Thinking;
    EXPECT_EQ(claudestate::display(r), Display::Thinking);
    r.base = ClaudeState::Compacting;
    EXPECT_EQ(claudestate::display(r), Display::Compacting);
    r.base = ClaudeState::ToolUse;
    EXPECT_EQ(claudestate::display(r), Display::ToolUseGeneric);
}

// INV-7 — Null-safety on forPid / forFocused.
TEST(ClaudeStateResolver, Inv7NullSafe) {
    // null tracker
    Resolved r1 = claudestate::forPid(nullptr, 1234);
    EXPECT_EQ(r1.base, ClaudeState::NotRunning);
    EXPECT_FALSE(r1.awaitingInput);

    ClaudeTabTracker tracker;
    // pid <= 0
    Resolved r2 = claudestate::forPid(&tracker, 0);
    EXPECT_EQ(r2.base, ClaudeState::NotRunning);
    Resolved r3 = claudestate::forPid(&tracker, -1);
    EXPECT_EQ(r3.base, ClaudeState::NotRunning);

    // forFocused null arguments
    Resolved r4 = claudestate::forFocused(nullptr, nullptr);
    EXPECT_EQ(r4.base, ClaudeState::NotRunning);
    Resolved r5 = claudestate::forFocused(&tracker, nullptr);
    EXPECT_EQ(r5.base, ClaudeState::NotRunning);
}

// INV-8 — claudestatuswidgets.cpp's apply() reads via the helper.
// The deleted scalars m_lastState / m_lastDetail / m_promptActive /
// m_planMode / m_auditing must not appear in the apply() body.
TEST(ClaudeStateResolver, Inv8ApplyUsesHelper) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_NE(src.find("claudestate::forFocused"), std::string::npos)
        << "INV-8: apply() must read via claudestate::forFocused";
    EXPECT_NE(src.find("claudestate::display("), std::string::npos)
        << "INV-8: apply() must use claudestate::display() ladder";

    // The deleted members must no longer be referenced (cache removed).
    // (The members themselves are removed from the header — checking
    //  the cpp catches any stray writes / reads.)
    EXPECT_EQ(src.find("m_lastState"), std::string::npos)
        << "INV-8: m_lastState must be deleted (helper now reads tracker)";
    EXPECT_EQ(src.find("m_lastDetail"), std::string::npos)
        << "INV-8: m_lastDetail must be deleted";
    EXPECT_EQ(src.find("m_promptActive"), std::string::npos)
        << "INV-8: m_promptActive must be deleted";
}

// INV-9 — tab-dot lambda also consumes the helper.
TEST(ClaudeStateResolver, Inv9TabDotUsesHelper) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_NE(src.find("claudestate::fromShell"), std::string::npos)
        << "INV-9: tab-dot lambda must adapt via claudestate::fromShell";
    // The same display() call services both surfaces; finding it twice
    // would be brittle, so we anchor on a single occurrence (above) and
    // assert the fromShell wiring is present here.
}

// INV-10 — tracker shellStateChanged is connected to a slot that calls
// apply() (so a tracker-driven state change re-paints the bar without
// waiting for an integration signal).
TEST(ClaudeStateResolver, Inv10ShellStateChangedTriggersApply) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);

    // The connect line must reference shellStateChanged AND apply.
    // We don't lock the exact lambda body shape — any path that gets
    // apply() called when the focused tab's tracker state changes
    // satisfies the invariant.
    const auto shellSig = src.find("shellStateChanged");
    ASSERT_NE(shellSig, std::string::npos)
        << "INV-10: a shellStateChanged connect must exist";

    // The same handler must invoke apply() — search for the pair
    // within a single 800-char window (a typical connect lambda).
    bool foundPair = false;
    const std::string needleApply = "apply()";
    for (size_t pos = 0;
         (pos = src.find("shellStateChanged", pos)) != std::string::npos;
         pos += 16) {
        const size_t end =
            std::min(pos + 800, src.size());
        if (src.find(needleApply, pos) < end) {
            foundPair = true;
            break;
        }
    }
    EXPECT_TRUE(foundPair)
        << "INV-10: shellStateChanged handler must call apply() so the "
           "status bar refreshes when the focused tab's tracker state "
           "changes (the bidirectional desync fix).";
}

// ANTS-5773 INV-1 — Background iff base == Idle, work is pending, and no
// overlay is set. With nothing pending every case reads as before.
TEST(ClaudeStateResolver, Ants5773BackgroundOnlyOverIdle) {
    struct Overlay { bool awaiting, plan, audit; };
    const Overlay overlays[] = {{false, false, false}, {true, false, false},
                                {false, true, false},  {false, false, true}};
    for (auto base : {ClaudeState::NotRunning, ClaudeState::Idle,
                      ClaudeState::Thinking, ClaudeState::ToolUse,
                      ClaudeState::Compacting}) {
        for (const Overlay &o : overlays) {
            Resolved r;
            r.base = base;
            r.tool = QStringLiteral("Read");
            r.awaitingInput = o.awaiting;
            r.planMode = o.plan;
            r.auditing = o.audit;
            const claudestate::Display without = claudestate::display(r);
            EXPECT_NE(without, Display::Background);
            r.backgroundShells = 2;
            r.backgroundPending = 2;
            const claudestate::Display with = claudestate::display(r);
            const bool expectBg = base == ClaudeState::Idle
                && !o.awaiting && !o.plan && !o.audit;
            if (expectBg) {
                EXPECT_EQ(without, Display::Idle);
                EXPECT_EQ(with, Display::Background);
            } else {
                EXPECT_EQ(with, without)
                    << "the count changed base=" << int(base)
                    << " awaiting=" << o.awaiting << " plan=" << o.plan
                    << " audit=" << o.audit;
            }
        }
    }
}

// ANTS-5773 INV-12 (guard) — ClaudeState gains no value, so tab_list,
// ModelAutoSwitch and idleSinceMs still see Idle for a waiting tab.
TEST(ClaudeStateResolver, Ants5773ClaudeStateUnchanged) {
    const std::string h = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const auto at = h.find("enum class ClaudeState");
    ASSERT_NE(at, std::string::npos);
    const auto open = h.find('{', at);
    const auto close = h.find('}', open);
    ASSERT_NE(close, std::string::npos);
    std::string body = h.substr(open + 1, close - open - 1);
    // Strip comments, then collect the identifiers.
    std::string names;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body.compare(i, 2, "//") == 0) {
            i = body.find('\n', i);
            if (i == std::string::npos) break;
            continue;
        }
        const char c = body[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') names += c;
        else if (c == ',' ) names += ',';
    }
    if (!names.empty() && names.back() == ',') names.pop_back();
    EXPECT_EQ(names, "NotRunning,Idle,Thinking,ToolUse,Compacting");
}
