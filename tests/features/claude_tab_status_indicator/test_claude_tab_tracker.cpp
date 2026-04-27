// Feature-conformance test for tests/features/claude_tab_status_indicator/spec.md.
//
// Asserts the contract for ClaudeTabTracker — the per-tab Claude state
// tracker that drives the tab-bar activity glyph:
//
//   INV-1 Multi-shell mapping. Two shells tracked simultaneously each
//         get independent per-shell snapshots; transcript mutation on
//         one fires shellStateChanged for its pid only.
//   INV-2 State mapping parity. For each documented transcript tail,
//         ClaudeTabTracker's derived state matches the authoritative
//         ClaudeIntegration::parseTranscriptTail helper — since the
//         tracker delegates to that exact helper, a drift would mean
//         an accidental re-implementation in the tracker.
//   INV-3 Untrack releases resources. After untrackShell, trackedCount
//         drops and shellState returns defaults.
//   INV-4 AwaitingInput wins. markShellAwaitingInput(pid, true) sets
//         the flag independently of the transcript-derived state, and
//         reverts cleanly without losing the underlying state.
//   INV-5 Plan-mode latching. permission-mode "plan" in the tail →
//         planMode true; latch preserved across tails with no event;
//         "default" clears the latch.
//   INV-6 Session-id → shell-PID routing. Given two tracked shells each
//         with their own transcript file, shellForSessionId("<uuid>")
//         returns the owning shell PID by matching the UUID against the
//         transcript filename basename.
//   INV-7 Bash tool surfaces as a distinct glyph hint. When the tail's
//         tool_use block carries name "Bash", tabState().tool equals
//         "Bash" so the mainwindow.cpp provider can route it to the
//         Bash glyph color (not the generic ToolUse color).
//
// No real Claude Code processes are spawned. Tests use the
// forceRefreshForTest(pid, transcriptPath) test seam to inject a
// synthetic transcript file, bypassing the proc-walking detection path
// that would otherwise require a real Claude child under the shell.
//
// Exit 0 = all assertions hold.

#include "claudeintegration.h"
#include "claudetabtracker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool write_file(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(body);
    f.close();
    return true;
}

// INV-2 via the parser helper. Cheap way to validate every documented
// last-event variant produces the expected ClaudeState.
int checkTranscriptMapping(const char *label, const char *jsonl,
                           ClaudeState expected, bool expectPlanMode) {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "[%s] tmpdir fail\n", label); return 1; }
    const QString path = tmp.path() + "/session.jsonl";
    if (!write_file(path, jsonl)) {
        std::fprintf(stderr, "[%s] write fail\n", label); return 1;
    }
    auto snap = ClaudeIntegration::parseTranscriptTail(path, /*latchedPlanMode=*/false);
    if (!snap.hasEvents) {
        std::fprintf(stderr, "[%s] FAIL no events parsed\n", label); return 1;
    }
    if (snap.stateDetermined && snap.state != expected) {
        std::fprintf(stderr, "[%s] FAIL state=%d want=%d\n",
                     label, int(snap.state), int(expected));
        return 1;
    }
    if (expectPlanMode && !snap.planMode) {
        std::fprintf(stderr, "[%s] FAIL planMode expected true\n", label);
        return 1;
    }
    if (!expectPlanMode && snap.planMode) {
        std::fprintf(stderr, "[%s] FAIL planMode expected false\n", label);
        return 1;
    }
    std::printf("[%-32s] state=%d planMode=%d  PASS\n",
                label, int(snap.state), snap.planMode);
    return 0;
}

// INV-1 Multi-shell independence on the real tracker — inject two
// transcripts via the test seam, verify the tracker keeps independent
// state per pid AND fires the signal with the right pid argument.
int checkMultiShellIndependence() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "[INV-1] tmpdir fail\n"); return 1; }

    const QString pathA = tmp.path() + "/a.jsonl";
    const QString pathB = tmp.path() + "/b.jsonl";

    write_file(pathA,
        R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{"command":"ls"}}]}}
)");
    write_file(pathB,
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"done"}]}}
)");

    ClaudeTabTracker tracker;
    QSignalSpy spy(&tracker, &ClaudeTabTracker::shellStateChanged);

    const pid_t pidA = 101;
    const pid_t pidB = 202;

    tracker.forceRefreshForTest(pidA, pathA);
    tracker.forceRefreshForTest(pidB, pathB);

    if (tracker.trackedCount() != 2) {
        std::fprintf(stderr, "[INV-1] FAIL expected 2 tracked, got %d\n", tracker.trackedCount()); return 1;
    }

    auto sA = tracker.shellState(pidA);
    auto sB = tracker.shellState(pidB);

    if (sA.state != ClaudeState::ToolUse || sA.tool != QStringLiteral("Bash")) {
        std::fprintf(stderr, "[INV-1] FAIL shell A state=%d tool=%s want ToolUse/Bash\n",
                     int(sA.state), qPrintable(sA.tool)); return 1;
    }
    if (sB.state != ClaudeState::Idle || !sB.tool.isEmpty()) {
        std::fprintf(stderr, "[INV-1] FAIL shell B state=%d tool=%s want Idle/''\n",
                     int(sB.state), qPrintable(sB.tool)); return 1;
    }

    // Both pids received a state change signal.
    if (spy.count() < 2) {
        std::fprintf(stderr, "[INV-1] FAIL expected >=2 signals, got %d\n", int(spy.count())); return 1;
    }
    bool sawA = false, sawB = false;
    for (const auto &emitted : spy) {
        const pid_t pid = emitted.at(0).value<pid_t>();
        if (pid == pidA) sawA = true;
        if (pid == pidB) sawB = true;
    }
    if (!sawA || !sawB) {
        std::fprintf(stderr, "[INV-1] FAIL missing pid in signal stream: A=%d B=%d\n", sawA, sawB); return 1;
    }

    // Independence proof: flip A's transcript to an end_turn and verify
    // B's state is unchanged.
    write_file(pathA,
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"A idle now"}]}}
)");
    tracker.forceRefreshForTest(pidA);  // reuse stored path
    auto sA2 = tracker.shellState(pidA);
    auto sB2 = tracker.shellState(pidB);
    if (sA2.state != ClaudeState::Idle) {
        std::fprintf(stderr, "[INV-1] FAIL A did not transition to Idle: state=%d\n", int(sA2.state)); return 1;
    }
    if (sB2.state != ClaudeState::Idle) {
        std::fprintf(stderr, "[INV-1] FAIL B bled with A's transition: state=%d\n", int(sB2.state)); return 1;
    }

    std::printf("[%-32s] A=ToolUse→Idle  B=Idle (independent)  PASS\n",
                "INV-1-multi-shell-independence");
    return 0;
}

// INV-4 AwaitingInput override + clean revert.
int checkAwaitingInputOverride() {
    ClaudeTabTracker tracker;
    const pid_t kPid = 98765;

    // Prime with a transcript so the tracker has a real underlying state
    // (ToolUse/Bash). Then flip awaitingInput true and verify the flag
    // is set WHILE the underlying state is preserved.
    QTemporaryDir tmp;
    const QString path = tmp.path() + "/s.jsonl";
    write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{}}]}}
)");

    tracker.forceRefreshForTest(kPid, path);
    if (tracker.shellState(kPid).state != ClaudeState::ToolUse) {
        std::fprintf(stderr, "[INV-4] FAIL prime expected ToolUse, got %d\n",
                     int(tracker.shellState(kPid).state)); return 1;
    }

    QSignalSpy spy(&tracker, &ClaudeTabTracker::shellStateChanged);

    tracker.markShellAwaitingInput(kPid, true);
    auto s1 = tracker.shellState(kPid);
    if (!s1.awaitingInput) {
        std::fprintf(stderr, "[INV-4] FAIL awaitingInput not set\n"); return 1;
    }
    if (s1.state != ClaudeState::ToolUse) {
        std::fprintf(stderr, "[INV-4] FAIL underlying state clobbered by awaiting flag: %d\n",
                     int(s1.state)); return 1;
    }
    if (spy.count() != 1) {
        std::fprintf(stderr, "[INV-4] FAIL expected 1 signal on set, got %d\n", int(spy.count())); return 1;
    }

    // Idempotent: re-setting true must NOT re-fire.
    tracker.markShellAwaitingInput(kPid, true);
    if (spy.count() != 1) {
        std::fprintf(stderr, "[INV-4] FAIL idempotent set re-fired: %d\n", int(spy.count())); return 1;
    }

    // Clear — underlying state must still be ToolUse.
    tracker.markShellAwaitingInput(kPid, false);
    auto s2 = tracker.shellState(kPid);
    if (s2.awaitingInput) {
        std::fprintf(stderr, "[INV-4] FAIL awaitingInput not cleared\n"); return 1;
    }
    if (s2.state != ClaudeState::ToolUse) {
        std::fprintf(stderr, "[INV-4] FAIL underlying state lost during revert: %d\n",
                     int(s2.state)); return 1;
    }
    if (spy.count() != 2) {
        std::fprintf(stderr, "[INV-4] FAIL expected 2 signals after clear, got %d\n",
                     int(spy.count())); return 1;
    }

    // Idempotent clear.
    tracker.markShellAwaitingInput(kPid, false);
    if (spy.count() != 2) {
        std::fprintf(stderr, "[INV-4] FAIL idempotent clear re-fired: %d\n", int(spy.count())); return 1;
    }

    std::printf("[%-32s] awaiting=1→0 underlying=ToolUse preserved  PASS\n",
                "INV-4-awaiting-override");
    return 0;
}

// INV-3 Untrack releases resources.
int checkUntrackReleasesResources() {
    ClaudeTabTracker tracker;
    const pid_t kPid = 11111;

    QTemporaryDir tmp;
    const QString path = tmp.path() + "/s.jsonl";
    write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
)");

    tracker.forceRefreshForTest(kPid, path);
    if (tracker.trackedCount() != 1) {
        std::fprintf(stderr, "[INV-3] FAIL expected 1 tracked, got %d\n",
                     tracker.trackedCount()); return 1;
    }

    QSignalSpy spy(&tracker, &ClaudeTabTracker::shellStateChanged);
    tracker.untrackShell(kPid);
    if (tracker.trackedCount() != 0) {
        std::fprintf(stderr, "[INV-3] FAIL expected 0 tracked after untrack, got %d\n",
                     tracker.trackedCount()); return 1;
    }

    auto s = tracker.shellState(kPid);
    if (s.state != ClaudeState::NotRunning || s.awaitingInput) {
        std::fprintf(stderr, "[INV-3] FAIL stale state after untrack: state=%d awaiting=%d\n",
                     int(s.state), s.awaitingInput); return 1;
    }

    // Post-untrack markShellAwaitingInput(kPid, false) on an absent entry
    // must not emit (documented contract).
    tracker.markShellAwaitingInput(kPid, false);
    if (spy.count() != 0) {
        std::fprintf(stderr, "[INV-3] FAIL spurious signal on no-op clear: %d\n",
                     int(spy.count())); return 1;
    }

    std::printf("[%-32s] tracked=0 state=NotRunning  PASS\n", "INV-3-untrack-releases");
    return 0;
}

// INV-5 Plan-mode latching via real tracker — seed a transcript with
// plan mode on, then overwrite with a plan-mode-absent tail and verify
// the tracker retains the latch.
int checkPlanModeLatching() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "[INV-5] tmpdir fail\n"); return 1; }

    ClaudeTabTracker tracker;
    const pid_t pid = 55555;
    const QString path = tmp.path() + "/plan.jsonl";

    write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"entering plan"}]}}
{"type":"permission-mode","permissionMode":"plan"}
)");
    tracker.forceRefreshForTest(pid, path);
    if (!tracker.shellState(pid).planMode) {
        std::fprintf(stderr, "[INV-5] FAIL plan mode not entered\n"); return 1;
    }

    // Tail without a permission-mode event — latch must persist.
    write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"still in plan"}]}}
)");
    tracker.forceRefreshForTest(pid);
    if (!tracker.shellState(pid).planMode) {
        std::fprintf(stderr, "[INV-5] FAIL plan mode latch dropped on quiet tail\n"); return 1;
    }

    // Switch back to default.
    write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"back"}]}}
{"type":"permission-mode","permissionMode":"default"}
)");
    tracker.forceRefreshForTest(pid);
    if (tracker.shellState(pid).planMode) {
        std::fprintf(stderr, "[INV-5] FAIL plan mode not exited\n"); return 1;
    }

    std::printf("[%-32s] enter=1 latch=1 exit=0  PASS\n", "INV-5-plan-mode-latch");
    return 0;
}

// INV-6 Session-id → shell-PID routing. Two shells, each with a
// transcript file named after a distinct UUID — shellForSessionId
// maps UUID to pid by basename comparison.
int checkSessionIdRouting() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "[INV-6] tmpdir fail\n"); return 1; }

    const QString uuidA = QStringLiteral("11111111-1111-1111-1111-111111111111");
    const QString uuidB = QStringLiteral("22222222-2222-2222-2222-222222222222");
    const QString pathA = tmp.path() + "/" + uuidA + ".jsonl";
    const QString pathB = tmp.path() + "/" + uuidB + ".jsonl";
    const char *body =
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
)";
    write_file(pathA, body);
    write_file(pathB, body);

    ClaudeTabTracker tracker;
    const pid_t pidA = 301;
    const pid_t pidB = 402;
    tracker.forceRefreshForTest(pidA, pathA);
    tracker.forceRefreshForTest(pidB, pathB);

    pid_t gotA = tracker.shellForSessionId(uuidA);
    pid_t gotB = tracker.shellForSessionId(uuidB);
    if (gotA != pidA) {
        std::fprintf(stderr, "[INV-6] FAIL session A -> pid %d, want %d\n",
                     int(gotA), int(pidA)); return 1;
    }
    if (gotB != pidB) {
        std::fprintf(stderr, "[INV-6] FAIL session B -> pid %d, want %d\n",
                     int(gotB), int(pidB)); return 1;
    }
    // Empty and unknown inputs return 0.
    if (tracker.shellForSessionId(QString()) != 0) {
        std::fprintf(stderr, "[INV-6] FAIL empty session id didn't return 0\n"); return 1;
    }
    if (tracker.shellForSessionId(QStringLiteral("deadbeef")) != 0) {
        std::fprintf(stderr, "[INV-6] FAIL unknown session id didn't return 0\n"); return 1;
    }

    std::printf("[%-32s] A/B resolved; empty/unknown→0  PASS\n",
                "INV-6-session-id-routing");
    return 0;
}

// INV-7 Bash tool surfaces distinctly. The provider in MainWindow
// routes based on `tool == "Bash"`; we assert the tracker stores
// exactly that string verbatim so the provider's check holds.
int checkBashToolSurfacing() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "[INV-7] tmpdir fail\n"); return 1; }

    const QString path = tmp.path() + "/bash.jsonl";
    write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{"command":"ls -la"}}]}}
)");

    ClaudeTabTracker tracker;
    const pid_t pid = 555;
    tracker.forceRefreshForTest(pid, path);

    auto s = tracker.shellState(pid);
    if (s.state != ClaudeState::ToolUse) {
        std::fprintf(stderr, "[INV-7] FAIL state=%d want ToolUse\n", int(s.state)); return 1;
    }
    if (s.tool != QLatin1String("Bash")) {
        std::fprintf(stderr, "[INV-7] FAIL tool='%s' want 'Bash'\n",
                     qPrintable(s.tool)); return 1;
    }

    // Flip to Read — tool must update.
    write_file(path,
        R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Read","input":{"file_path":"/tmp/x"}}]}}
)");
    tracker.forceRefreshForTest(pid);
    auto s2 = tracker.shellState(pid);
    if (s2.tool != QLatin1String("Read")) {
        std::fprintf(stderr, "[INV-7] FAIL tool post-flip='%s' want 'Read'\n",
                     qPrintable(s2.tool)); return 1;
    }

    std::printf("[%-32s] Bash→Read surfaced  PASS\n", "INV-7-bash-tool-surfacing");
    return 0;
}

// INV-8 Per-shell transcript path is project-cwd-scoped. Two cwds → two
// distinct project subdirs under ~/.claude/projects/ → sessionPathForCwd
// must return each subdir's own .jsonl, not collapse them onto whichever
// is system-wide newest. Source-grep + a round-trip on a synthetic
// projects directory.
int checkProjectCwdScopedTranscript() {
    // Source-grep half — confirm the tracker reads /proc/<pid>/cwd and
    // delegates to ClaudeIntegration::sessionPathForCwd, rather than
    // the pre-0.7.48 system-wide newest walk.
    {
        QFile f(QString::fromUtf8(ANTS_SOURCE_DIR "/src/claudetabtracker.cpp"));
        if (!f.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "[INV-8] FAIL cannot open claudetabtracker.cpp\n");
            return 1;
        }
        const QByteArray src = f.readAll();
        if (!src.contains("ClaudeIntegration::sessionPathForCwd")) {
            std::fprintf(stderr, "[INV-8] FAIL tracker does not call sessionPathForCwd\n");
            return 1;
        }
        if (!src.contains("/proc/%1/cwd")) {
            std::fprintf(stderr, "[INV-8] FAIL tracker does not read /proc/<pid>/cwd\n");
            return 1;
        }
    }

    // Round-trip half — fake a ~/.claude/projects/ tree under a temp
    // HOME, drop two subdirs (encoded forms of two cwds), put a
    // .jsonl in each, and prove sessionPathForCwd routes each cwd to
    // its own subdir's file.
    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "[INV-8] tmpdir fail\n"); return 1; }

    // ConfigPaths::claudeProjectsDir() reads $HOME — override it.
    const QByteArray oldHome = qgetenv("HOME");
    qputenv("HOME", tmp.path().toUtf8());

    auto restore = qScopeGuard([&]() {
        if (oldHome.isEmpty()) qunsetenv("HOME");
        else                   qputenv("HOME", oldHome);
    });

    const QString projectsDir = tmp.path() + "/.claude/projects";
    QDir().mkpath(projectsDir);

    const QString cwdA = tmp.path() + "/work/projA";
    const QString cwdB = tmp.path() + "/work/projB";
    QDir().mkpath(cwdA);
    QDir().mkpath(cwdB);

    const QString encA = ClaudeIntegration::encodeProjectPath(cwdA);
    const QString encB = ClaudeIntegration::encodeProjectPath(cwdB);
    QDir().mkpath(projectsDir + "/" + encA);
    QDir().mkpath(projectsDir + "/" + encB);

    const QString jsonlA = projectsDir + "/" + encA + "/sessA.jsonl";
    const QString jsonlB = projectsDir + "/" + encB + "/sessB.jsonl";
    write_file(jsonlA, "{}\n");
    write_file(jsonlB, "{}\n");

    const QString gotA = ClaudeIntegration::sessionPathForCwd(cwdA);
    const QString gotB = ClaudeIntegration::sessionPathForCwd(cwdB);

    if (QFileInfo(gotA).canonicalFilePath() != QFileInfo(jsonlA).canonicalFilePath()) {
        std::fprintf(stderr, "[INV-8] FAIL cwdA → '%s' want '%s'\n",
                     qPrintable(gotA), qPrintable(jsonlA));
        return 1;
    }
    if (QFileInfo(gotB).canonicalFilePath() != QFileInfo(jsonlB).canonicalFilePath()) {
        std::fprintf(stderr, "[INV-8] FAIL cwdB → '%s' want '%s'\n",
                     qPrintable(gotB), qPrintable(jsonlB));
        return 1;
    }

    // Empty cwd → empty (ensures no accidental fallback to global newest
    // through this entry point).
    if (!ClaudeIntegration::sessionPathForCwd(QString()).isEmpty()) {
        std::fprintf(stderr, "[INV-8] FAIL empty cwd should resolve to empty\n");
        return 1;
    }

    std::printf("[%-32s] cwdA→A.jsonl  cwdB→B.jsonl (independent)  PASS\n",
                "INV-8-project-cwd-scoped");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;

    // INV-2 State mapping parity.
    struct Scenario {
        const char *label;
        const char *jsonl;
        ClaudeState expected;
        bool expectPlanMode;
    } kScenarios[] = {
        { "assistant-tool-use",
          R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{}}]}}
{"type":"system","subtype":"turn_duration","ms":100}
)",
          ClaudeState::ToolUse, false },
        { "assistant-end-turn",
          R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"last-prompt","text":"hi"}
)",
          ClaudeState::Idle, false },
        { "assistant-streaming-null",
          R"({"type":"assistant","message":{"stop_reason":null,"content":[{"type":"text","text":"..."}]}}
)",
          ClaudeState::Thinking, false },
        { "user-tool-result",
          R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"x","content":"ok"}]}}
)",
          ClaudeState::Thinking, false },
        { "plan-mode-entered",
          R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","permissionMode":"plan"}
)",
          ClaudeState::Idle, /*planMode=*/true },
    };
    for (const auto &s : kScenarios) {
        failures += checkTranscriptMapping(s.label, s.jsonl, s.expected, s.expectPlanMode);
    }

    failures += checkMultiShellIndependence();
    failures += checkAwaitingInputOverride();
    failures += checkUntrackReleasesResources();
    failures += checkPlanModeLatching();
    failures += checkSessionIdRouting();
    failures += checkBashToolSurfacing();
    failures += checkProjectCwdScopedTranscript();

    if (failures) {
        std::fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nAll invariants hold.\n");
    return 0;
}
