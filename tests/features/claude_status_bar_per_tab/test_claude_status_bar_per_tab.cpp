// Feature-conformance test for spec.md — asserts ANTS-1161:
// the singleton ClaudeIntegration's hook dispatch must drop state
// mutations for session_ids that don't match the focused tab.
//
// Drives processHookEventForTest directly (test-only accessor),
// same pattern as claude_status_bar's parseTranscriptForState.
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include "claudeintegration.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
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

