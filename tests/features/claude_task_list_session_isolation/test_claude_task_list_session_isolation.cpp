// Feature-conformance test for ANTS-1219 — wiring contract between
// the claude_session_freshness resolver (activeSessionPath) and the
// claude_task_list parser (parseTranscript), via the
// ClaudeStatusBarController on the 2 s status-timer cadence.
//
// INV labels qualified ANTS-1219-INV-N. See spec.md.
//
// Test shape (source-grep, no GUI, no QTemporaryDir, no instantiation):
//  * Slurp src/claudestatuswidgets.cpp + src/mainwindow.cpp.
//  * For each INV, assert the load-bearing string is present, scoped
//    to the right function body where the spec demands locality.
//  * The asserted strings are anchored to `// ANTS-1219-INV-N`
//    comments next to each one in src/, so the grep is intentional —
//    a refactor that moves the comment also has to move the wiring it
//    documents, otherwise this test fails.
//
// Why this exists: lock the resolver↔parser wiring so a future
// refactor that drops the path push, the timer cadence, the tab-switch
// reset, the feedback connect, or the poll-rescue surfaces as a red
// build (a specific INV violation) rather than the user-visible
// "stale tasks from a prior session" symptom that triggered ANTS-1219.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const std::string &detail = {}) {
    std::fprintf(stderr, "[%-72s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 detail.empty() ? "" : " — ",
                 detail.c_str());
    if (!ok) ++g_failures;
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Return the substring of `src` from the first occurrence of
// `sigPrefix` up to (but not including) the next top-level `^}` line
// that closes the function. Falls back to a generous slice if the
// brace-balance walk never returns to depth 0 (defensive — should not
// happen for well-formed C++).
std::string functionBody(const std::string &src,
                         const std::string &sigPrefix) {
    const auto start = src.find(sigPrefix);
    if (start == std::string::npos) return {};
    // Find first '{' after the signature and walk braces.
    auto open = src.find('{', start);
    if (open == std::string::npos) return src.substr(start, 4096);
    int depth = 0;
    size_t i = open;
    for (; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) { ++i; break; }
        }
    }
    return src.substr(start, i - start);
}

// True iff `needle` appears inside the body of the named function in
// `src`. Used to enforce locality (e.g. "this string MUST be inside
// refreshTasksButton, not just somewhere in the file").
bool bodyContains(const std::string &src,
                  const std::string &sigPrefix,
                  const std::string &needle) {
    const std::string body = functionBody(src, sigPrefix);
    return body.find(needle) != std::string::npos;
}

void testInv1RefreshWiring(const std::string &widgets) {
    // INV-1: refreshTasksButton must (a) call activeSessionPath on the
    // focused-tab cwd, (b) gate the push on path != prevPath, (c) push
    // the resolver result to the tracker via setTranscriptPath. All
    // three sites must live inside refreshTasksButton — locality
    // matters because if any moves out of the timer-driven path, the
    // 2 s propagation contract (INV-2) breaks silently.
    const std::string sig =
        "void ClaudeStatusBarController::refreshTasksButton(";

    expect(bodyContains(widgets, sig, "// ANTS-1219-INV-1"),
           "ANTS-1219-INV-1: anchor comment present in refreshTasksButton");
    expect(bodyContains(widgets, sig,
                        "m_integration->activeSessionPath(cwd)"),
           "ANTS-1219-INV-1: activeSessionPath(cwd) called in refreshTasksButton");
    expect(bodyContains(widgets, sig, "if (path != prevPath)"),
           "ANTS-1219-INV-1: path-change gate present in refreshTasksButton");
    expect(bodyContains(widgets, sig,
                        "m_tasks->setTranscriptPath(path)"),
           "ANTS-1219-INV-1: setTranscriptPath(path) push in refreshTasksButton");
}

void testInv2TimerCadence(const std::string &mainwindow) {
    // INV-2: 2 s cadence + connect that fires refreshTasksButton on
    // every tick. The interval IS the worst-case latency for a
    // resolver-swap propagating to the chip. The connect pulls that
    // propagation onto the timer instead of leaving it gated on a
    // file-change event the watcher might have lost.
    expect(mainwindow.find("// ANTS-1219-INV-2") != std::string::npos,
           "ANTS-1219-INV-2: anchor comment present in mainwindow.cpp");
    expect(mainwindow.find("m_statusTimer->setInterval(2000)") != std::string::npos,
           "ANTS-1219-INV-2: m_statusTimer interval is 2000 ms");
    // The connect spans multiple lines in current source — match on
    // the unique RHS of the connect, which only occurs in this one site.
    expect(mainwindow.find("&ClaudeStatusBarController::refreshTasksButton)") != std::string::npos,
           "ANTS-1219-INV-2: refreshTasksButton timer connect present");
}

void testInv3HideBranch(const std::string &widgets) {
    // INV-3: empty-resolver propagation. INV-1's setTranscriptPath
    // push handles empty `path` identically (the parser-side ANTS-1158
    // INV-7 clears m_tasks). The visible side is the chip hide branch
    // — `total <= 0 || unfinished <= 0` at claudestatuswidgets.cpp:705
    // — which fires when the cleared tracker reports both as 0.
    const std::string sig =
        "void ClaudeStatusBarController::refreshTasksButton(";
    expect(bodyContains(widgets, sig, "// ANTS-1219-INV-3"),
           "ANTS-1219-INV-3: anchor comment present in refreshTasksButton");
    expect(bodyContains(widgets, sig, "unfinished <= 0"),
           "ANTS-1219-INV-3: chip hide branch (unfinished <= 0) present");
}

void testInv4TabSwitchReset(const std::string &widgets,
                            const std::string &mainwindow) {
    // INV-4: tab-switch reset clears the tracker's bound path
    // synchronously, and the call site fires from MainWindow's
    // tab-change handler. Both halves must be present.
    const std::string sig =
        "void ClaudeStatusBarController::resetForTabSwitch(";
    expect(bodyContains(widgets, sig, "// ANTS-1219-INV-4"),
           "ANTS-1219-INV-4: anchor comment present in resetForTabSwitch");
    expect(bodyContains(widgets, sig,
                        "m_tasks->setTranscriptPath(QString())"),
           "ANTS-1219-INV-4: setTranscriptPath(QString()) clears tracker on tab switch");

    expect(mainwindow.find("// ANTS-1219-INV-4") != std::string::npos,
           "ANTS-1219-INV-4: anchor comment present in mainwindow.cpp");
    expect(mainwindow.find(
               "m_claudeStatusBarController->resetForTabSwitch()") != std::string::npos,
           "ANTS-1219-INV-4: resetForTabSwitch invoked from MainWindow");
}

void testInv5FeedbackConnect(const std::string &widgets) {
    // INV-5: tasksChanged → refreshTasksButton feedback connect. The
    // two `connect()` arguments are on adjacent lines in current
    // source; assert both halves are present and that they share the
    // same INV-5 anchor.
    expect(widgets.find("// ANTS-1219-INV-5") != std::string::npos,
           "ANTS-1219-INV-5: anchor comment present in claudestatuswidgets.cpp");
    expect(widgets.find(
               "connect(m_tasks, &ClaudeTaskListTracker::tasksChanged") != std::string::npos,
           "ANTS-1219-INV-5: tasksChanged signal LHS of feedback connect");
    expect(widgets.find(
               "&ClaudeStatusBarController::refreshTasksButton)") != std::string::npos,
           "ANTS-1219-INV-5: refreshTasksButton slot RHS of feedback connect");

    // Locality: the LHS and RHS must be near each other (same
    // connect call). Within 200 chars covers both line-break styles.
    const auto lhs = widgets.find(
        "connect(m_tasks, &ClaudeTaskListTracker::tasksChanged");
    const auto rhs = widgets.find(
        "&ClaudeStatusBarController::refreshTasksButton)", lhs);
    const bool adjacent =
        lhs != std::string::npos && rhs != std::string::npos &&
        rhs > lhs && (rhs - lhs) < 200;
    expect(adjacent,
           "ANTS-1219-INV-5: tasksChanged and refreshTasksButton are in the same connect()",
           "lhs=" + std::to_string(lhs) + " rhs=" + std::to_string(rhs));
}

void testInv6PollRescue(const std::string &widgets) {
    // INV-6: poll-rescue call. m_tasks->poll() must be inside
    // refreshTasksButton (not a separate slot, not behind a flag) so
    // every status tick re-checks the JSONL mtime — the rescue path
    // for QFileSystemWatcher dropping its watch on Claude's
    // tmpfile+rename atomic rewrite.
    const std::string sig =
        "void ClaudeStatusBarController::refreshTasksButton(";
    expect(bodyContains(widgets, sig, "// ANTS-1219-INV-6"),
           "ANTS-1219-INV-6: anchor comment present in refreshTasksButton");
    expect(bodyContains(widgets, sig, "m_tasks->poll()"),
           "ANTS-1219-INV-6: m_tasks->poll() called in refreshTasksButton");
}

}  // namespace

TEST(claude_task_list_session_isolation, Inv1RefreshWiring) {
    const std::string widgets = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    int before = g_failures;
    testInv1RefreshWiring(widgets);
    if (g_failures > before) FAIL();
}

TEST(claude_task_list_session_isolation, Inv2TimerCadence) {
    const std::string mainwindow = slurp(SRC_MAINWINDOW_CPP_PATH);
    int before = g_failures;
    testInv2TimerCadence(mainwindow);
    if (g_failures > before) FAIL();
}

TEST(claude_task_list_session_isolation, Inv3HideBranch) {
    const std::string widgets = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    int before = g_failures;
    testInv3HideBranch(widgets);
    if (g_failures > before) FAIL();
}

TEST(claude_task_list_session_isolation, Inv4TabSwitchReset) {
    const std::string widgets = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string mainwindow = slurp(SRC_MAINWINDOW_CPP_PATH);
    int before = g_failures;
    testInv4TabSwitchReset(widgets, mainwindow);
    if (g_failures > before) FAIL();
}

TEST(claude_task_list_session_isolation, Inv5FeedbackConnect) {
    const std::string widgets = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    int before = g_failures;
    testInv5FeedbackConnect(widgets);
    if (g_failures > before) FAIL();
}

TEST(claude_task_list_session_isolation, Inv6PollRescue) {
    const std::string widgets = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    int before = g_failures;
    testInv6PollRescue(widgets);
    if (g_failures > before) FAIL();
}
