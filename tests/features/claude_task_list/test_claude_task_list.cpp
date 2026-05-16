// Feature-conformance test for ANTS-1158 — Claude Code task-list
// status-bar surface. Hybrid: link claudetasklist.cpp + run the parser
// against inline JSONL fixtures (INV-1..8); source-grep the wiring
// sites for INV-9..13.
//
// INV labels qualified ANTS-1158-INV-N. See spec.md.

#include "../../_support/expect.h"
#include "claudetasklist.h"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {



std::string readFile(const char *path) {
    // ANTS-1217 Phase 4: bundles run from build/, so prefix with the
    // CMake-baked source root if the path is relative.
#ifdef ANTS_SOURCE_DIR
    std::string fullpath;
    if (path && path[0] != '/') {
        fullpath = std::string(ANTS_SOURCE_DIR) + "/" + path;
        path = fullpath.c_str();
    }
#endif
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

// ----- ANTS-1221: unfinishedCount excludes in_progress -----
//
// Pre-fix contract: `unfinishedCount() = pending + in_progress`.
// User repro 2026-05-10 (screenshot): "41 tasks — 40 done, 1 running,
// 0 outstanding" but chip read "☰ 1/41". A single in_flight task kept
// the chip lit even though *the user* had nothing to action.
// Fix-a (chosen): `unfinishedCount()` counts `pending` only — chip
// surfaces user-actionable work, not Claude's in-flight work.
void testAnts1221Inv1_unfinishedExcludesInProgress() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1221-INV-1 setup"); return; }

    // Mixed-status fixture: 1 completed + 1 in_progress + 2 pending.
    const QString p = writeFixture(dir, "fix_1221.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"Done","status":"completed","activeForm":"Doing done"},)"
            R"({"content":"Working","status":"in_progress","activeForm":"Working"},)"
            R"({"content":"Todo A","status":"pending","activeForm":"Doing A"},)"
            R"({"content":"Todo B","status":"pending","activeForm":"Doing B"})"
            R"(]})"),
    });

    ClaudeTaskListTracker tracker;
    tracker.setTranscriptPath(p);

    expect(tracker.totalCount() == 4,
           "ANTS-1221-INV-1: tracker sees all 4 tasks",
           "got total=" + std::to_string(tracker.totalCount()));
    expect(tracker.pendingCount() == 2,
           "ANTS-1221-INV-1: pendingCount counts only pending entries",
           "got pending=" + std::to_string(tracker.pendingCount()));
    expect(tracker.inProgressCount() == 1,
           "ANTS-1221-INV-1: inProgressCount counts only in_progress entries",
           "got in_progress=" + std::to_string(tracker.inProgressCount()));
    expect(tracker.unfinishedCount() == tracker.pendingCount(),
           "ANTS-1221-INV-1: unfinishedCount() == pendingCount() — running excluded",
           "got unfinished=" + std::to_string(tracker.unfinishedCount()) +
           " pending=" + std::to_string(tracker.pendingCount()));
}

// All-running fixture. The contract evolved across three IDs:
// • ANTS-1221 (pending-only counter, 2026-05-10): an all-running
//   list yielded `unfinished == 0`, so the chip *hid* — that was
//   the resolution for "running keeps me out of the user's
//   actionable count" but it left the user unable to see progress
//   when only running tasks remained.
// • ANTS-1246 (progress counter, 2026-05-12): chip now reads
//   `done/total` and shows iff `0 < done < total`. An all-running
//   list has `done == 0 < total`, so the chip *stays visible* at
//   "☰ 0/N" — closing the residual hole. The tracker's
//   `pendingCount()` is still 0 (preserved for tooltip detail),
//   but it no longer drives visibility.
void testAnts1221Inv2_allRunningChipStaysVisible() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1221/1246 setup"); return; }

    const QString p = writeFixture(dir, "fix_1221_running.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"Working A","status":"in_progress","activeForm":"A"},)"
            R"({"content":"Working B","status":"in_progress","activeForm":"B"})"
            R"(]})"),
    });

    ClaudeTaskListTracker tracker;
    tracker.setTranscriptPath(p);

    expect(tracker.totalCount() == 2,
           "ANTS-1221/1246: tracker sees both running tasks");
    expect(tracker.pendingCount() == 0,
           "ANTS-1221/1246: all-running list has no pending tasks",
           "got pending=" + std::to_string(tracker.pendingCount()));
    expect(tracker.completedCount() == 0,
           "ANTS-1246: all-running list has 0 completed (chip shows ☰ 0/N, "
           "stays visible — closes the hole left by 1221's pending-only "
           "visibility predicate)",
           "got done=" + std::to_string(tracker.completedCount()) +
           " total=" + std::to_string(tracker.totalCount()));
}

// ----- ANTS-1218 / ANTS-1246: chip X/Y reads done-of-total -----
//
// Contract history:
// • Pre-1218: `arg(unfinished).arg(total)` — chip counted DOWN
//   ("☰ 6/30" = "6 left of 30"), but X/Y is universally read
//   as "X done of Y".
// • ANTS-1218 (2026-05-08): switched to `arg(total - unfinished).arg(total)`
//   — counted UP. With ANTS-1221 making unfinished = pending only,
//   the numerator effectively meant `done + in_progress`.
// • ANTS-1246 (2026-05-12): switched to `arg(done).arg(total)` —
//   pure progress counter. In_progress no longer rolls into the
//   numerator; chip stays visible the whole run.
//
// This test now pins the ANTS-1246 numerator. ANTS-1218's
// "counts up not down" intent is preserved a fortiori (going
// 0 → 1 → … → N done is monotone-up).
void testAnts1218Inv1_chipFormatCountsUp() {
    const std::string csw = readFile("src/claudestatuswidgets.cpp");
    if (csw.empty()) {
        expect(false, "ANTS-1218-INV-1 read claudestatuswidgets.cpp");
        return;
    }
    const auto pos = csw.find("m_tasksBtn->setText");
    bool ok = false;
    if (pos != std::string::npos) {
        const auto end = std::min(pos + 400, csw.size());
        const std::string near = csw.substr(pos, end - pos);
        // ANTS-1246: numerator must be `done`. Reject the legacy
        // `total - unfinished` form (which dropped in_progress into
        // the numerator and hid the chip when only in_progress
        // remained — the bug ANTS-1246 closes).
        ok = contains(near, "arg(done)") && contains(near, "arg(total)");
    }
    expect(ok,
           "ANTS-1218/1246-INV-1: chip numerator is `done` "
           "(arg(done).arg(total)) — pure progress counter");
}

// Behavioural cross-check: simulate the chip's arithmetic against
// three fixture states walking from "all pending" → "one running" →
// "one done". The displayed numerator must monotonically rise.
//
// Memory note: one tracker re-pointed three times, not three trackers.
// Each ClaudeTaskListTracker owns a QFileSystemWatcher (one inotify
// watch); three trackers would mean three concurrent watches for a
// purely sequential check.
void testAnts1218Inv2_chipNumeratorMonotone() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1218-INV-2 setup"); return; }

    // State A: 3 pending, 0 running, 0 done → numerator = 0
    const QString pA = writeFixture(dir, QStringLiteral("a.jsonl"), {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"X","status":"pending","activeForm":"X"},)"
            R"({"content":"Y","status":"pending","activeForm":"Y"},)"
            R"({"content":"Z","status":"pending","activeForm":"Z"})"
            R"(]})"),
    });
    // State B: 2 pending, 1 running, 0 done → numerator = 1 (post-1221:
    // running counts toward the "not user-actionable" half)
    const QString pB = writeFixture(dir, QStringLiteral("b.jsonl"), {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"X","status":"in_progress","activeForm":"X"},)"
            R"({"content":"Y","status":"pending","activeForm":"Y"},)"
            R"({"content":"Z","status":"pending","activeForm":"Z"})"
            R"(]})"),
    });
    // State C: 2 pending, 0 running, 1 done → numerator = 1
    const QString pC = writeFixture(dir, QStringLiteral("c.jsonl"), {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"X","status":"completed","activeForm":"X"},)"
            R"({"content":"Y","status":"pending","activeForm":"Y"},)"
            R"({"content":"Z","status":"pending","activeForm":"Z"})"
            R"(]})"),
    });

    // ANTS-1246: numerator is `completedCount()` (pure progress).
    // Pre-1246 used `totalCount() - unfinishedCount()` (= done +
    // in_progress under post-1221). Walking pending → in_progress
    // no longer bumps the chip; only completion does.
    ClaudeTaskListTracker tracker;
    tracker.setTranscriptPath(pA);
    const int totalA = tracker.totalCount();
    const int nA = tracker.completedCount();
    tracker.setTranscriptPath(pB);
    const int totalB = tracker.totalCount();
    const int nB = tracker.completedCount();
    tracker.setTranscriptPath(pC);
    const int totalC = tracker.totalCount();
    const int nC = tracker.completedCount();

    expect(nA == 0,
           "ANTS-1218/1246-INV-2: 3 pending → numerator 0 (no done)",
           "got " + std::to_string(nA));
    expect(nB == nA,
           "ANTS-1246-INV-2: pending → in_progress does NOT change the "
           "numerator (was nA+1 under pre-1246; now stays nA because "
           "only completed counts)",
           "A=" + std::to_string(nA) + " B=" + std::to_string(nB));
    expect(nC > nB,
           "ANTS-1218/1246-INV-2: pending → completed bumps the numerator",
           "B=" + std::to_string(nB) + " C=" + std::to_string(nC));
    expect(nA <= totalA && nB <= totalB && nC <= totalC,
           "ANTS-1218/1246-INV-2: numerator never exceeds total");
}

// Build an `isCompactSummary:true` event line. Always type==user in
// current Claude Code; carries the rolled-up summary text. Parser must
// treat it as a state-reset checkpoint — events before contribute zero
// task entries to the post-checkpoint state.
QString compactSummaryEvent() {
    return QStringLiteral(
        R"({"type":"user","isSidechain":false,"isCompactSummary":true,"message":{"role":"user","content":"compacted summary text"}})");
}

// ANTS-1327 supersedes ANTS-1224: isCompactSummary is a NO-OP marker,
// NOT a state-reset checkpoint. The chip and dialog therefore mirror
// Claude Code's own sidebar — full session history across /compact.

void testAnts1327Inv1_compactSummaryPreservesState() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1327-INV-1 setup"); return; }

    // Pre-compact: TodoWrite with 2 tasks. Then isCompactSummary
    // (NO-OP marker — must NOT clear state). Then post-compact:
    // TodoWrite with 1 task. The post-compact TodoWrite REPLACES the
    // pre-compact one (TodoWrite is snapshot-replace per existing
    // semantics) — that's INV-3's standing behaviour, not 1327's.
    // INV-1 checks that the parser doesn't crash or zero out anything
    // unexpectedly when it sees the compact marker.
    const QString p = writeFixture(dir, "fix1327a.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"Pre-compact A","status":"pending","activeForm":"Doing A"},)"
            R"({"content":"Pre-compact B","status":"pending","activeForm":"Doing B"})"
            R"(]})",
            QStringLiteral("toolu_pre")),
        compactSummaryEvent(),
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"Post-compact only","status":"in_progress","activeForm":"Doing post"})"
            R"(]})",
            QStringLiteral("toolu_post")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    // TodoWrite is replace-semantics — post-compact TodoWrite still
    // wins over pre-compact, regardless of the compact marker.
    expect(tasks.size() == 1,
           "ANTS-1327-INV-1: TodoWrite-after-compact still replaces (existing INV-3 behaviour)",
           "got " + std::to_string(tasks.size()));
    if (tasks.size() == 1) {
        expect(tasks[0].subject.contains(QStringLiteral("Post-compact only")),
               "ANTS-1327-INV-1: surviving entry is from the last TodoWrite");
    }
}

void testAnts1327Inv1b_taskCreateBeforeCompactSurvives() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1327-INV-1b setup"); return; }

    // ANTS-1327 reversal repro: TaskCreate before /compact, then
    // isCompactSummary, then nothing post-compact. Under the old
    // ANTS-1224 contract this would yield zero tasks (compact cleared
    // state). Under 1327 the pre-compact tasks MUST survive.
    const QString p = writeFixture(dir, "fix1327b.jsonl", {
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Test the chip","description":"x","activeForm":"x"})",
            QStringLiteral("toolu_t1")),
        userToolResult(QStringLiteral("toolu_t1"),
            QStringLiteral("Task #1 created successfully: Test the chip")),
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Run /compact","description":"x","activeForm":"x"})",
            QStringLiteral("toolu_t2")),
        userToolResult(QStringLiteral("toolu_t2"),
            QStringLiteral("Task #2 created successfully: Run /compact")),
        compactSummaryEvent(),
        // No post-compact task events.
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 2,
           "ANTS-1327-INV-1: TaskCreate events before isCompactSummary survive — chip mirrors CC sidebar full-history view",
           "got " + std::to_string(tasks.size()));
}

void testAnts1327Inv2_multipleCheckpointsAllNoop() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1327-INV-2 setup"); return; }

    // Two checkpoints, each a no-op. The three TodoWrites are each
    // snapshot-replace, so the final TodoWrite wins — but the
    // compact markers must NOT have introduced any side effect.
    const QString p = writeFixture(dir, "fix1327c.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[{"content":"Phase 1","status":"pending","activeForm":"P1"}]})",
            QStringLiteral("toolu_p1")),
        compactSummaryEvent(),
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[{"content":"Phase 2","status":"pending","activeForm":"P2"}]})",
            QStringLiteral("toolu_p2")),
        compactSummaryEvent(),
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[{"content":"Phase 3","status":"in_progress","activeForm":"P3"}]})",
            QStringLiteral("toolu_p3")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1327-INV-2: with replace-semantic TodoWrite, last TodoWrite still wins regardless of compact markers",
           "got " + std::to_string(tasks.size()));
    if (tasks.size() == 1) {
        expect(tasks[0].subject.contains(QStringLiteral("Phase 3")),
               "ANTS-1327-INV-2: surviving entry is the final TodoWrite (replace-semantic INV-3, unaffected by compact)");
    }
}

void testAnts1327Inv3_sidechainCompactStillFiltered() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1327-INV-3 setup"); return; }

    // Sidechain CompactSummary must be filtered by INV-5 sidechain
    // skip BEFORE the INV-1 no-op fires. Behaviour-wise this is
    // identical to the deprecated 1224-INV-3 — the order matters
    // even though INV-1's body changed from clear-and-continue to
    // continue.
    const QString sidechainCheckpoint = QStringLiteral(
        R"({"type":"user","isSidechain":true,"isCompactSummary":true,"message":{"role":"user","content":"side"}})");
    const QString p = writeFixture(dir, "fix1327d.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[{"content":"Parent","status":"pending","activeForm":"P"}]})",
            QStringLiteral("toolu_parent")),
        sidechainCheckpoint,  // filtered by INV-5; must NOT affect state
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1327-INV-3: sidechain isCompactSummary does not reach the INV-1 skip "
           "(INV-5 sidechain filter fires first)",
           "got " + std::to_string(tasks.size()));
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

// ANTS-1246-INV-6: batch-reset on TaskCreate after all-completed.
// Synthetic transcript: [TaskCreate A, tool_result id=7, TaskUpdate
// A→completed, TaskCreate B]. Pre-fix Mode B would yield
// totalCount() == 2 (A and B both kept). Post-fix the all-completed
// prior batch is dropped when B's TaskCreate arrives → totalCount() == 1.
void testAnts1246Inv6_batchResetAfterAllCompleted() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1246-INV-6 setup"); return; }

    const QString p = writeFixture(dir, "fix1246a.jsonl", {
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Old A","description":"d","activeForm":"A"})",
            QStringLiteral("toolu_a")),
        userToolResult(QStringLiteral("toolu_a"),
            QStringLiteral("Task #7 created successfully: Old A")),
        assistantToolUse(QStringLiteral("TaskUpdate"),
            R"({"taskId":"7","status":"completed"})",
            QStringLiteral("toolu_au")),
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"New B","description":"d","activeForm":"B"})",
            QStringLiteral("toolu_b")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1246-INV-6: batch-reset dropped the all-completed "
           "prior batch (expected just B)",
           "got " + std::to_string(tasks.size()) + " task(s)");
    if (tasks.size() == 1) {
        expect(tasks[0].subject == QStringLiteral("New B"),
               "ANTS-1246-INV-6: surviving task is B (the new batch), "
               "not A (old completed)");
        expect(tasks[0].status == QStringLiteral("pending"),
               "ANTS-1246-INV-6: surviving task is in pending status "
               "(fresh, not stale-completed)");
    }
}

// ANTS-1246-INV-7: partial batch is preserved — if any prior task
// is pending/in_progress, a new TaskCreate appends without clearing.
// Sequence: A pending, B pending, B→completed, C pending →
// totalCount() == 3 (A pending, B completed, C pending).
void testAnts1246Inv7_partialBatchPreserved() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1246-INV-7 setup"); return; }

    const QString p = writeFixture(dir, "fix1246b.jsonl", {
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"A","description":"d","activeForm":"A"})",
            QStringLiteral("toolu_a")),
        userToolResult(QStringLiteral("toolu_a"),
            QStringLiteral("Task #1 created successfully: A")),
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"B","description":"d","activeForm":"B"})",
            QStringLiteral("toolu_b")),
        userToolResult(QStringLiteral("toolu_b"),
            QStringLiteral("Task #2 created successfully: B")),
        assistantToolUse(QStringLiteral("TaskUpdate"),
            R"({"taskId":"2","status":"completed"})",
            QStringLiteral("toolu_bu")),
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"C","description":"d","activeForm":"C"})",
            QStringLiteral("toolu_c")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 3,
           "ANTS-1246-INV-7: partial batch preserved — A pending blocks "
           "the reset; expected 3 tasks",
           "got " + std::to_string(tasks.size()));
}

// ANTS-1246-INV-8: TodoWrite (Mode A) snapshot-replace path unchanged
// by this spec. Two TodoWrites in sequence: the second wins, the
// first's tasks are dropped (existing behavior, regression-pinned).
void testAnts1246Inv8_modeASnapshotUnchanged() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1246-INV-8 setup"); return; }

    const QString p = writeFixture(dir, "fix1246c.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"Old 1","status":"completed","activeForm":"1"},)"
            R"({"content":"Old 2","status":"completed","activeForm":"2"})"
            R"(]})"),
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"New","status":"pending","activeForm":"N"})"
            R"(]})"),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1246-INV-8: TodoWrite Mode A snapshot-replace still "
           "drops prior batch — Mode A path unchanged");
    if (tasks.size() == 1) {
        expect(tasks[0].subject == QStringLiteral("New") ||
               tasks[0].subject.contains(QStringLiteral("New")),
               "ANTS-1246-INV-8: surviving task is the new TodoWrite's content");
    }
}

// ANTS-1407-INV-1: an empty TodoWrite does NOT lock Mode B. The
// parser preserves `sawTodoWrite = false` when `todos: []`, so a
// subsequent TaskCreate is honoured rather than silently dropped.
void testAnts1407Inv1_emptyTodoWriteDoesNotLockModeB() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1407-INV-1 setup"); return; }

    const QString p = writeFixture(dir, "fix1407a.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[]})",
            QStringLiteral("toolu_empty")),
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Post-empty","description":"d","activeForm":"P"})",
            QStringLiteral("toolu_post")),
        userToolResult(QStringLiteral("toolu_post"),
            QStringLiteral("Task #99 created successfully: Post-empty")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1407-INV-1: empty TodoWrite did not lock Mode B; "
           "the subsequent TaskCreate is honoured",
           "got " + std::to_string(tasks.size()));
    if (tasks.size() == 1) {
        expect(tasks[0].subject == QStringLiteral("Post-empty"),
               "ANTS-1407-INV-1: surviving task is from the post-empty TaskCreate");
        expect(tasks[0].status == QStringLiteral("pending"),
               "ANTS-1407-INV-1: status is pending (fresh TaskCreate)");
    }
}

// ANTS-1407-INV-3 + INV-4: a `TaskCreate` arriving after a prior
// batch whose only task is `deleted` (a terminal status post-1407)
// fires the widened batch-reset, then the final-pass deleted-filter
// strips A entirely.
void testAnts1407Inv3_batchResetWithDeleted() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1407-INV-3 setup"); return; }

    const QString p = writeFixture(dir, "fix1407b.jsonl", {
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Old A","description":"d","activeForm":"A"})",
            QStringLiteral("toolu_a")),
        userToolResult(QStringLiteral("toolu_a"),
            QStringLiteral("Task #1 created successfully: Old A")),
        assistantToolUse(QStringLiteral("TaskUpdate"),
            R"({"taskId":"1","status":"deleted"})",
            QStringLiteral("toolu_adel")),
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"New B","description":"d","activeForm":"B"})",
            QStringLiteral("toolu_b")),
        userToolResult(QStringLiteral("toolu_b"),
            QStringLiteral("Task #2 created successfully: New B")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1407-INV-3: batch-reset fires on `deleted` prior "
           "batch (widened terminal predicate); deleted A then "
           "stripped by final filter; only B remains",
           "got " + std::to_string(tasks.size()));
    if (tasks.size() == 1) {
        expect(tasks[0].subject == QStringLiteral("New B"),
               "ANTS-1407-INV-3: surviving task is B");
        expect(tasks[0].status == QStringLiteral("pending"),
               "ANTS-1407-INV-3: B is pending (fresh)");
    }
}

// ANTS-1407-INV-4: a task with status `deleted` at end of parse is
// removed from the returned list (final-pass filter).
void testAnts1407Inv4_deletedFiltered() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1407-INV-4 setup"); return; }

    const QString p = writeFixture(dir, "fix1407c.jsonl", {
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"Doomed","description":"d","activeForm":"D"})",
            QStringLiteral("toolu_doom")),
        userToolResult(QStringLiteral("toolu_doom"),
            QStringLiteral("Task #7 created successfully: Doomed")),
        assistantToolUse(QStringLiteral("TaskUpdate"),
            R"({"taskId":"7","status":"deleted"})",
            QStringLiteral("toolu_doomdel")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 0,
           "ANTS-1407-INV-4: deleted task removed from final list",
           "got " + std::to_string(tasks.size()));
}

// ANTS-1407-INV-7: Mode A then empty TodoWrite then Mode B —
// the empty TodoWrite clears the lock, so the subsequent
// TaskCreate is honoured.
void testAnts1407Inv7_modeAThenModeBViaEmpty() {
    QTemporaryDir dir;
    if (!dir.isValid()) { expect(false, "ANTS-1407-INV-7 setup"); return; }

    const QString p = writeFixture(dir, "fix1407d.jsonl", {
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[)"
            R"({"content":"A1","status":"pending","activeForm":"a1"},)"
            R"({"content":"B1","status":"pending","activeForm":"b1"})"
            R"(]})",
            QStringLiteral("toolu_w1")),
        assistantToolUse(QStringLiteral("TodoWrite"),
            R"({"todos":[]})",
            QStringLiteral("toolu_w2")),
        assistantToolUse(QStringLiteral("TaskCreate"),
            R"({"subject":"C2","description":"d","activeForm":"c2"})",
            QStringLiteral("toolu_c")),
        userToolResult(QStringLiteral("toolu_c"),
            QStringLiteral("Task #3 created successfully: C2")),
    });

    const auto tasks = ClaudeTaskListTracker::parseTranscript(p);
    expect(tasks.size() == 1,
           "ANTS-1407-INV-7: empty TodoWrite reset the Mode-A lock; "
           "subsequent TaskCreate is honoured; just C remains",
           "got " + std::to_string(tasks.size()));
    if (tasks.size() == 1) {
        expect(tasks[0].subject == QStringLiteral("C2"),
               "ANTS-1407-INV-7: surviving task is the post-empty TaskCreate");
    }
}

}  // namespace


TEST(ClaudeTaskList, Inv1TodoWriteSnapshot) {
    const int before = expect_failures();
    testInv1_todoWriteSnapshot();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv2MostRecentWins) {
    const int before = expect_failures();
    testInv2_mostRecentWins();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv3TaskCreatePairedResult) {
    const int before = expect_failures();
    testInv3_taskCreatePairedResult();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv4TaskUpdateFlipsStatus) {
    const int before = expect_failures();
    testInv4_taskUpdateFlipsStatus();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv5SidechainFiltered) {
    const int before = expect_failures();
    testInv5_sidechainFiltered();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv6SubagentDispatchFiltered) {
    const int before = expect_failures();
    testInv6_subagentDispatchFiltered();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv7SetEmptyPathClears) {
    const int before = expect_failures();
    testInv7_setEmptyPathClears();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv8SetSamePathIdempotent) {
    const int before = expect_failures();
    testInv8_setSamePathIdempotent();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv9WidgetHiddenOnEmpty) {
    const int before = expect_failures();
    testInv9_widgetHiddenOnEmpty();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv10WidgetLabelFormat) {
    const int before = expect_failures();
    testInv10_widgetLabelFormat();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv11DialogRendersRows) {
    const int before = expect_failures();
    testInv11_dialogRendersRows();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv12DialogRebuildsOnTasksChanged) {
    const int before = expect_failures();
    testInv12_dialogRebuildsOnTasksChanged();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Inv13DialogAntiRegressionWaylandFlake) {
    const int before = expect_failures();
    testInv13_dialogAntiRegressionWaylandFlake();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1221Inv1UnfinishedExcludesInProgress) {
    const int before = expect_failures();
    testAnts1221Inv1_unfinishedExcludesInProgress();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1221Inv2AllRunningChipStaysVisible) {
    const int before = expect_failures();
    testAnts1221Inv2_allRunningChipStaysVisible();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1218Inv1ChipFormatCountsUp) {
    const int before = expect_failures();
    testAnts1218Inv1_chipFormatCountsUp();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1327Inv1CompactSummaryPreservesState) {
    const int before = expect_failures();
    testAnts1327Inv1_compactSummaryPreservesState();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1327Inv1bTaskCreateBeforeCompactSurvives) {
    const int before = expect_failures();
    testAnts1327Inv1b_taskCreateBeforeCompactSurvives();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1327Inv2MultipleCheckpointsAllNoop) {
    const int before = expect_failures();
    testAnts1327Inv2_multipleCheckpointsAllNoop();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1327Inv3SidechainCompactStillFiltered) {
    const int before = expect_failures();
    testAnts1327Inv3_sidechainCompactStillFiltered();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1246Inv6BatchResetAfterAllCompleted) {
    const int before = expect_failures();
    testAnts1246Inv6_batchResetAfterAllCompleted();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1246Inv7PartialBatchPreserved) {
    const int before = expect_failures();
    testAnts1246Inv7_partialBatchPreserved();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1246Inv8ModeASnapshotUnchanged) {
    const int before = expect_failures();
    testAnts1246Inv8_modeASnapshotUnchanged();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1218Inv2ChipNumeratorMonotone) {
    const int before = expect_failures();
    testAnts1218Inv2_chipNumeratorMonotone();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1407Inv1EmptyTodoWriteDoesNotLockModeB) {
    const int before = expect_failures();
    testAnts1407Inv1_emptyTodoWriteDoesNotLockModeB();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1407Inv3BatchResetWithDeleted) {
    const int before = expect_failures();
    testAnts1407Inv3_batchResetWithDeleted();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1407Inv4DeletedFiltered) {
    const int before = expect_failures();
    testAnts1407Inv4_deletedFiltered();
    if (expect_failures() > before) FAIL();
}

TEST(ClaudeTaskList, Ants1407Inv7ModeAThenModeBViaEmpty) {
    const int before = expect_failures();
    testAnts1407Inv7_modeAThenModeBViaEmpty();
    if (expect_failures() > before) FAIL();
}

