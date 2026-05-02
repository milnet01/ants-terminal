// Feature-conformance test for ANTS-1158 — Claude Code task-list
// status-bar surface. Hybrid: link claudetasklist.cpp + run the parser
// against inline JSONL fixtures (INV-1..8); source-grep the wiring
// sites for INV-9..13.
//
// INV labels qualified ANTS-1158-INV-N. See spec.md.

#include "claudetasklist.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

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

std::string readFile(const char *path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Write a JSONL fixture into a temp file inside `dir`. Returns the
// file path. Each `lines` element is one event (one JSONL line).
QString writeFixture(QTemporaryDir &dir,
                     const QString &name,
                     const QStringList &lines) {
    const QString path = dir.path() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    for (const QString &l : lines) {
        f.write(l.toUtf8());
        f.write("\n");
    }
    f.close();
    return path;
}

// Build a `tool_use` event in the assistant role. `name` is the tool
// name; `inputJson` is the raw JSON object body for `input`.
QString assistantToolUse(const QString &name,
                         const QString &inputJson,
                         const QString &toolUseId = QStringLiteral("toolu_test"),
                         bool isSidechain = false) {
    return QStringLiteral(
        R"({"type":"assistant","isSidechain":%1,"message":{"role":"assistant","content":[{"type":"tool_use","id":"%2","name":"%3","input":%4}]}})")
        .arg(isSidechain ? "true" : "false", toolUseId, name, inputJson);
}

// Build a `user` role event carrying a `tool_result` for the paired
// tool_use_id. `resultBody` is the result content string.
QString userToolResult(const QString &toolUseId,
                       const QString &resultBody) {
    return QStringLiteral(
        R"({"type":"user","isSidechain":false,"message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"%1","content":"%2"}]}})")
        .arg(toolUseId, resultBody);
}

// ----- Parser lane (link-based) -----

void testInv1_todoWriteSnapshot() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-1 setup"); return; }

    const QString p = writeFixture(dir, "fix1.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"Build the thing","status":"completed","activeForm":"Building the thing"},)"
            R"({"content":"Test the thing","status":"in_progress","activeForm":"Testing the thing"},)"
            R"({"content":"Ship the thing","status":"pending","activeForm":"Shipping the thing"})"
            R"(]})"),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 3,
           "ANTS-1158-INV-1: TodoWrite snapshot produces 3 tasks",
           "got " + std::to_string(tasks.size()));
    if (tasks.size() == 3) {
        expect(tasks[0].status == QStringLiteral("completed")
            && tasks[1].status == QStringLiteral("in_progress")
            && tasks[2].status == QStringLiteral("pending"),
            "ANTS-1158-INV-1: TodoWrite preserves status across all 3 entries");
    }
}

void testInv2_mostRecentWins() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-2 setup"); return; }

    const QString p = writeFixture(dir, "fix2.jsonl", {
        // Older snapshot — 2 tasks
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"Old A","status":"completed","activeForm":"Doing A"},)"
            R"({"content":"Old B","status":"pending","activeForm":"Doing B"})"
            R"(]})",
            QStringLiteral("toolu_old")),
        // Newer snapshot — 1 task
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"New only","status":"in_progress","activeForm":"Doing new"})"
            R"(]})",
            QStringLiteral("toolu_new")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1158-INV-2: most-recent TodoWrite wins (older 2-task list discarded)",
           "got " + std::to_string(tasks.size()));
    if (tasks.size() == 1) {
        expect(tasks[0].subject.contains(QStringLiteral("New only")),
               "ANTS-1158-INV-2: surviving task is from the latest snapshot");
    }
}

void testInv3_taskCreatePairedResult() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-3 setup"); return; }

    const QString p = writeFixture(dir, "fix3.jsonl", {
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Refactor the parser","description":"Split the lexer from the consumer","activeForm":"Refactoring the parser"})",
            QStringLiteral("toolu_a")),
        userToolResult(QStringLiteral("toolu_a"),
            QStringLiteral("Task #42 created successfully: Refactor the parser")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1158-INV-3: TaskCreate produces one entry");
    if (!tasks.isEmpty()) {
        expect(tasks[0].status == QStringLiteral("pending"),
               "ANTS-1158-INV-3: TaskCreate's initial status is pending");
        expect(tasks[0].subject == QStringLiteral("Refactor the parser"),
               "ANTS-1158-INV-3: subject preserved from input");
        expect(tasks[0].id == QStringLiteral("42"),
               "ANTS-1158-INV-3: ID extracted from paired tool_result body");
    }
}

void testInv4_taskUpdateFlipsStatus() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-4 setup"); return; }

    const QString p = writeFixture(dir, "fix4.jsonl", {
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Step one","description":"do it","activeForm":"Doing step one"})",
            QStringLiteral("toolu_c")),
        userToolResult(QStringLiteral("toolu_c"),
            QStringLiteral("Task #7 created successfully: Step one")),
        assistantToolUse(QStringLiteral("TaskUpdate"),
            R"({"taskId":"7","status":"in_progress"})",
            QStringLiteral("toolu_u1")),
        // No-op update: unknown taskId
        assistantToolUse(QStringLiteral("TaskUpdate"),
            R"({"taskId":"999","status":"completed"})",
            QStringLiteral("toolu_u2")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1158-INV-4: unknown-taskId TaskUpdate doesn't add a row");
    if (!tasks.isEmpty()) {
        expect(tasks[0].status == QStringLiteral("in_progress"),
               "ANTS-1158-INV-4: TaskUpdate flipped status to in_progress");
    }
}

void testInv5_sidechainFiltered() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-5 setup"); return; }

    const QString p = writeFixture(dir, "fix5.jsonl", {
        // Sidechain TaskCreate — must be ignored
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Subagent's own task","description":"x","activeForm":"x"})",
            QStringLiteral("toolu_sc"),
            /*isSidechain=*/true),
        userToolResult(QStringLiteral("toolu_sc"),
            QStringLiteral("Task #100 created successfully: Subagent's own task")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.isEmpty(),
           "ANTS-1158-INV-5: sidechain TaskCreate produces zero plan rows",
           "got " + std::to_string(tasks.size()));
}

void testInv6_subagentDispatchFiltered() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-6 setup"); return; }

    const QString p = writeFixture(dir, "fix6.jsonl", {
        // `Task` tool_use with subagent_type — subagent dispatch, NOT
        // a plan-list-add. Must be filtered out.
        assistantToolUse(QStringLiteral("Task"),
            R"({"subagent_type":"general-purpose","prompt":"go do something","description":"thing"})",
            QStringLiteral("toolu_sd")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.isEmpty(),
           "ANTS-1158-INV-6: Task tool_use with subagent_type is not a plan row",
           "got " + std::to_string(tasks.size()));
}

void testInv7_setEmptyPathClears() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-7 setup"); return; }
    const QString p = writeFixture(dir, "fix7.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[{"content":"x","status":"pending","activeForm":"X"}]})"),
    });

    ClaudeTaskListTracker tracker;
    QSignalSpy spy(&tracker, &ClaudeTaskListTracker::tasksChanged);
    tracker.setTranscriptPath(p);
    const int afterSet = spy.count();
    tracker.setTranscriptPath(QString{});
    const int afterClear = spy.count();

    expect(afterSet >= 1,
           "ANTS-1158-INV-7: initial set emits tasksChanged");
    expect(afterClear == afterSet + 1 && tracker.tasks().isEmpty(),
           "ANTS-1158-INV-7: setTranscriptPath(\"\") clears state and emits once",
           "afterSet=" + std::to_string(afterSet) +
           " afterClear=" + std::to_string(afterClear));
}

void testInv8_setSamePathIdempotent() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1158-INV-8 setup"); return; }
    const QString p = writeFixture(dir, "fix8.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[{"content":"x","status":"pending","activeForm":"X"}]})"),
    });

    ClaudeTaskListTracker tracker;
    tracker.setTranscriptPath(p);
    QSignalSpy spy(&tracker, &ClaudeTaskListTracker::tasksChanged);
    tracker.setTranscriptPath(p);
    expect(spy.count() == 0,
           "ANTS-1158-INV-8: re-set with same path is a no-op (no tasksChanged)",
           "got " + std::to_string(spy.count()) + " emit(s)");
}

// ----- Wiring lane (source-grep) -----

void testInv9_widgetHiddenOnEmpty() {
    const std::string csw = readFile("src/claudestatuswidgets.cpp");
    if (csw.empty()) {
        expect(false, "ANTS-1158-INV-9 read claudestatuswidgets.cpp");
        return;
    }
    // The refresh function must reference both an empty/zero check and
    // setVisible(false) (or hide()). Heuristic: any of "setVisible(false)",
    // "->hide()", "tasksBtn->hide" co-located with empty/0 check.
    const bool hasHide =
        contains(csw, "m_tasksBtn") &&
        (contains(csw, "m_tasksBtn->hide") ||
         contains(csw, "m_tasksBtn->setVisible(false)"));
    expect(hasHide,
           "ANTS-1158-INV-9: claudestatuswidgets.cpp hides m_tasksBtn on empty list");
}

void testInv10_widgetLabelFormat() {
    const std::string csw = readFile("src/claudestatuswidgets.cpp");
    if (csw.empty()) {
        expect(false, "ANTS-1158-INV-10 read claudestatuswidgets.cpp");
        return;
    }
    // Look for a setText that contains a "/" between two arg() calls or
    // a QString format with %1/%2 — the chip is "<unfinished>/<total>".
    // Heuristic: m_tasksBtn->setText present AND a "/" character used
    // within a QString format that mentions %1 and %2 in the same area.
    const auto pos = csw.find("m_tasksBtn->setText");
    bool ok = false;
    if (pos != std::string::npos) {
        const auto end = std::min(pos + 400, csw.size());
        const std::string near = csw.substr(pos, end - pos);
        ok = contains(near, "%1") && contains(near, "%2") && contains(near, "/");
    }
    expect(ok,
           "ANTS-1158-INV-10: m_tasksBtn label uses <unfinished>/<total> format");
}

void testInv11_dialogRendersRows() {
    const std::string dlg = readFile("src/claudetasklistdialog.cpp");
    if (dlg.empty()) {
        expect(false, "ANTS-1158-INV-11 read claudetasklistdialog.cpp");
        return;
    }
    // Heuristic: rebuild() body iterates m_tracker->tasks() and adds
    // rows to a QListWidget (addItem) per task.
    expect(contains(dlg, "QListWidget") || contains(dlg, "addItem"),
           "ANTS-1158-INV-11: dialog renders rows via QListWidget/addItem");
    expect(contains(dlg, "tasks()") || contains(dlg, "->tasks"),
           "ANTS-1158-INV-11: dialog reads tracker->tasks()");
}

void testInv12_dialogRebuildsOnTasksChanged() {
    const std::string dlg = readFile("src/claudetasklistdialog.cpp");
    if (dlg.empty()) {
        expect(false, "ANTS-1158-INV-12 read claudetasklistdialog.cpp");
        return;
    }
    // Connect tasksChanged → rebuild (or scheduleRebuild) somewhere
    // in the dialog source.
    const bool wired =
        contains(dlg, "tasksChanged") &&
        (contains(dlg, "rebuild") || contains(dlg, "scheduleRebuild"));
    expect(wired,
           "ANTS-1158-INV-12: dialog connects tracker tasksChanged to rebuild slot");
}

void testInv13_dialogAntiRegressionWaylandFlake() {
    const std::string dlg = readFile("src/claudetasklistdialog.cpp");
    const std::string dlgh = readFile("src/claudetasklistdialog.h");
    if (dlg.empty() || dlgh.empty()) {
        expect(false, "ANTS-1158-INV-13 read claudetasklistdialog.{cpp,h}");
        return;
    }
    expect(!contains(dlg, "setModal(true)") && !contains(dlgh, "setModal(true)"),
           "ANTS-1158-INV-13: dialog does NOT call setModal(true) "
           "(QTBUG-79126 click-drop on Wayland)");
    // Constructor-call check: anti-regression should fire on actual
    // instantiation, not any comment that mentions the class. Look
    // for `new QDialogButtonBox` or `QDialogButtonBox(` (an
    // instantiation expression, not a documentation token).
    const bool dlgInstantiates =
        contains(dlg, "new QDialogButtonBox")
        || contains(dlg, "QDialogButtonBox(")
        || contains(dlgh, "new QDialogButtonBox")
        || contains(dlgh, "QDialogButtonBox(");
    expect(!dlgInstantiates,
           "ANTS-1158-INV-13: dialog does NOT instantiate QDialogButtonBox "
           "(QTBUG-79126 click-drop on Wayland)");
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    testInv1_todoWriteSnapshot();
    testInv2_mostRecentWins();
    testInv3_taskCreatePairedResult();
    testInv4_taskUpdateFlipsStatus();
    testInv5_sidechainFiltered();
    testInv6_subagentDispatchFiltered();
    testInv7_setEmptyPathClears();
    testInv8_setSamePathIdempotent();

    testInv9_widgetHiddenOnEmpty();
    testInv10_widgetLabelFormat();
    testInv11_dialogRendersRows();
    testInv12_dialogRebuildsOnTasksChanged();
    testInv13_dialogAntiRegressionWaylandFlake();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nAll INVs PASS.\n");
    return 0;
}
