// Feature-conformance test for spec.md — asserts ANTS-1161:
// the singleton ClaudeIntegration's hook dispatch must drop state
// mutations for session_ids that don't match the focused tab.
//
// Drives processHookEventForTest directly (test-only accessor),
// same pattern as claude_status_bar's parseTranscriptForState.
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include "claudeintegration.h"
#include "claudestateresolver.h"
#include "claudetabtracker.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>
#include <QStringLiteral>

#include <cstdio>

namespace {

constexpr const char *kFocusedSession = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
constexpr const char *kSiblingSession = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";

QJsonObject preToolUse(const QString &sessionId, const QString &tool) {
    QJsonObject ev;
    ev.insert("hook_event_name", QStringLiteral("PreToolUse"));
    ev.insert("session_id", sessionId);
    ev.insert("tool_name", tool);
    ev.insert("tool_input", QJsonObject{});
    return ev;
}

QJsonObject permissionRequest(const QString &sessionId, const QString &tool) {
    QJsonObject ev;
    ev.insert("hook_event_name", QStringLiteral("PermissionRequest"));
    ev.insert("session_id", sessionId);
    ev.insert("tool_name", tool);
    QJsonObject input;
    input.insert("command", QStringLiteral("rm -rf /tmp/x"));
    ev.insert("tool_input", input);
    return ev;
}

QJsonObject sessionStart(const QString &sessionId) {
    QJsonObject ev;
    ev.insert("hook_event_name", QStringLiteral("SessionStart"));
    ev.insert("session_id", sessionId);
    return ev;
}

// Seed the integration with a focused-tab transcript path whose
// basename equals kFocusedSession. Doesn't need to point at a real
// file — only the basename is compared.
ClaudeIntegration *makeFocused() {
    auto *ci = new ClaudeIntegration();
    ci->setTranscriptPathForTest(
        QStringLiteral("/tmp/projects/dummy/") +
        QString::fromLatin1(kFocusedSession) +
        QStringLiteral(".jsonl"));
    return ci;
}

int runForeignSessionIsDropped() {
    // I1 — sibling tab's PreToolUse must not mutate state or emit
    // stateChanged on the singleton.
    auto *ci = makeFocused();
    QSignalSpy stateSpy(ci, &ClaudeIntegration::stateChanged);
    QSignalSpy toolSpy(ci, &ClaudeIntegration::toolStarted);

    const ClaudeState beforeState = ci->currentState();
    const QString beforeTool = ci->currentTool();

    ci->processHookEventForTest(preToolUse(kSiblingSession, "Bash"));
    QCoreApplication::processEvents();

    const bool stateUnchanged = ci->currentState() == beforeState;
    const bool toolUnchanged = ci->currentTool() == beforeTool;
    const bool noStateSignal = stateSpy.count() == 0;
    const bool noToolSignal = toolSpy.count() == 0;

    const bool ok = stateUnchanged && toolUnchanged && noStateSignal && noToolSignal;
    std::fprintf(stderr,
        "[I1 foreign-dropped]   stateUnchanged=%d toolUnchanged=%d "
        "noStateSig=%d noToolSig=%d  %s\n",
        stateUnchanged, toolUnchanged, noStateSignal, noToolSignal,
        ok ? "PASS" : "FAIL");

    delete ci;
    return ok ? 0 : 1;
}

int runFocusedSessionIsHonoured() {
    // I2 — focused tab's PreToolUse must mutate state and emit
    // stateChanged with the right tool name.
    auto *ci = makeFocused();
    QSignalSpy stateSpy(ci, &ClaudeIntegration::stateChanged);
    QSignalSpy toolSpy(ci, &ClaudeIntegration::toolStarted);

    ci->processHookEventForTest(preToolUse(kFocusedSession, "Read"));
    QCoreApplication::processEvents();

    const bool stateOk = ci->currentState() == ClaudeState::ToolUse;
    const bool toolOk = ci->currentTool() == QStringLiteral("Read");
    const bool stateSignalFired = stateSpy.count() == 1;
    const bool toolSignalFired = toolSpy.count() == 1;

    const bool ok = stateOk && toolOk && stateSignalFired && toolSignalFired;
    std::fprintf(stderr,
        "[I2 focused-honoured]  state=%d tool='%s' stateSig=%d toolSig=%d  %s\n",
        (int)ci->currentState(), qUtf8Printable(ci->currentTool()),
        (int)stateSpy.count(), (int)toolSpy.count(),
        ok ? "PASS" : "FAIL");

    delete ci;
    return ok ? 0 : 1;
}

int runPermissionRequestUngated() {
    // I3 — permission prompts must fire regardless of session_id, since
    // the slot routes via lastHookSessionId() to the per-tab tracker.
    auto *ci = makeFocused();
    QSignalSpy permSpy(ci, &ClaudeIntegration::permissionRequested);

    ci->processHookEventForTest(permissionRequest(kSiblingSession, "Bash"));
    QCoreApplication::processEvents();

    const bool fired = permSpy.count() == 1;
    const bool sidStashed = ci->lastHookSessionId() == QString::fromLatin1(kSiblingSession);

    const bool ok = fired && sidStashed;
    std::fprintf(stderr,
        "[I3 perm-ungated]      fired=%d sidStashed=%d  %s\n",
        fired, sidStashed, ok ? "PASS" : "FAIL");

    delete ci;
    return ok ? 0 : 1;
}

int runPrePollTolerance() {
    // I4 — when m_transcriptPath is empty (Claude bound but transcript
    // not yet resolved), accept events so the bootstrap SessionStart
    // can wire m_activeSessionId. Without this carve-out the gate
    // would drop the very first event a fresh Claude session emits.
    auto *ci = new ClaudeIntegration();   // no setTranscriptPathForTest
    QSignalSpy startSpy(ci, &ClaudeIntegration::sessionStarted);
    QSignalSpy stateSpy(ci, &ClaudeIntegration::stateChanged);

    ci->processHookEventForTest(sessionStart(kFocusedSession));
    QCoreApplication::processEvents();

    const bool startFired = startSpy.count() == 1;
    const bool stateFired = stateSpy.count() == 1;
    const bool stateOk = ci->currentState() == ClaudeState::Idle;

    const bool ok = startFired && stateFired && stateOk;
    std::fprintf(stderr,
        "[I4 pre-poll-tolerant] startSig=%d stateSig=%d state=%d  %s\n",
        startFired, stateFired, (int)ci->currentState(),
        ok ? "PASS" : "FAIL");

    delete ci;
    return ok ? 0 : 1;
}

int runColdStartPermissionRequestDropped() {
    // I5 (ANTS-2190) — during cold-start (m_transcriptPath empty,
    // isFocusedTabSession() returns true for ANY session_id) a
    // PermissionRequest cannot be attributed to the focused tab, so it must be
    // DROPPED rather than fired + committed to m_lastHookSessionId. Pre-fix it
    // self-routed via the poisoned last-seen session field, mis-attributing a
    // sibling tab's prompt to the focused tab when two Claude tabs are live.
    auto *ci = new ClaudeIntegration();   // cold-start: no transcript path set
    QSignalSpy permSpy(ci, &ClaudeIntegration::permissionRequested);

    ci->processHookEventForTest(permissionRequest(kSiblingSession, "Bash"));
    QCoreApplication::processEvents();

    const bool notFired = permSpy.count() == 0;
    const bool sidNotStashed = ci->lastHookSessionId().isEmpty();

    const bool ok = notFired && sidNotStashed;
    std::fprintf(stderr,
        "[I5 coldstart-perm-drop] notFired=%d sidNotStashed=%d  %s\n",
        notFired, sidNotStashed, ok ? "PASS" : "FAIL");

    delete ci;
    return ok ? 0 : 1;
}

}  // namespace


TEST(ClaudeStatusBarPerTab, ForeignSessionIsDropped) {
    ASSERT_EQ(runForeignSessionIsDropped(), 0);
}

TEST(ClaudeStatusBarPerTab, FocusedSessionIsHonoured) {
    ASSERT_EQ(runFocusedSessionIsHonoured(), 0);
}

TEST(ClaudeStatusBarPerTab, PermissionRequestUngated) {
    ASSERT_EQ(runPermissionRequestUngated(), 0);
}

TEST(ClaudeStatusBarPerTab, PrePollTolerance) {
    ASSERT_EQ(runPrePollTolerance(), 0);
}

TEST(ClaudeStatusBarPerTab, ColdStartPermissionRequestDropped) {
    ASSERT_EQ(runColdStartPermissionRequestDropped(), 0);
}


// ANTS-5773 INV-9 — each tracked shell's counts come from its own
// transcript: a tab that was never focused still shows Background.
TEST(ClaudeStatusBarPerTab, Ants5773UnfocusedTabCountsItsOwnTasks) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("bwmysc2h1.output"));
    {
        QFile f(out);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x\n");
    }
    // A timed-out shell moved to the background, then the turn ends.
    const QString busy = dir.filePath(QStringLiteral("busy.jsonl"));
    {
        QFile f(busy);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(
            R"({"type":"assistant","timestamp":"2026-10-19T10:31:21.000Z","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","id":"toolu_017V","input":{"command":"cc-job wait build","timeout":600000}}]}})" "\n"
            R"({"type":"user","timestamp":"2026-10-19T10:41:21.109Z","toolUseResult":{"backgroundTaskId":"bwmysc2h1","timedOutAfterMs":600000},"message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_017V","content":"Command did not complete within its 600s timeout and was moved to the background (ID: bwmysc2h1). Output is being written to: )");
        f.write(out.toUtf8());
        f.write(
            R"(. You will be notified when it completes."}]}})" "\n"
            R"({"type":"assistant","timestamp":"2026-10-19T10:41:30.000Z","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Waiting on the build."}]}})" "\n");
    }
    const QString quiet = dir.filePath(QStringLiteral("quiet.jsonl"));
    {
        QFile f(quiet);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"({"type":"assistant","timestamp":"2026-10-19T10:41:30.000Z","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Done."}]}})" "\n");
    }

    ClaudeTabTracker tracker;
    const pid_t focusedPid = 990001, otherPid = 990002;
    tracker.forceRefreshForTest(focusedPid, quiet);
    tracker.forceRefreshForTest(otherPid, busy);   // never focused

    const auto other = tracker.shellState(otherPid);
    EXPECT_EQ(other.backgroundShells, 1);
    EXPECT_EQ(claudestate::display(claudestate::fromShell(other)),
              claudestate::Display::Background);
    EXPECT_EQ(tracker.transcriptPath(otherPid), busy);

    const auto focused = tracker.shellState(focusedPid);
    EXPECT_EQ(focused.backgroundShells, 0);
    EXPECT_EQ(claudestate::display(claudestate::fromShell(focused)),
              claudestate::Display::Idle);
}
