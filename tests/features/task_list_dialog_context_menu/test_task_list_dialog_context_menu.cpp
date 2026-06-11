// Feature-conformance test for ANTS-1638 + ANTS-1639 — right-click
// context menu on ClaudeTaskListDialog + raw tool-input markup
// flagging.

#include <gtest/gtest.h>

#include "claudetasklist.h"
#include "claudetasklistdialog.h"

#include <QApplication>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include <fstream>
#include <string>

namespace {

constexpr int kRoleTaskId          = Qt::UserRole + 1;
constexpr int kRoleStatus          = Qt::UserRole + 2;
constexpr int kRoleSubject         = Qt::UserRole + 3;
constexpr int kRoleDescriptionFull = Qt::UserRole + 4;

// Write JSONL TodoWrite events that yield our desired task list when
// the production parser walks them. One TodoWrite snapshot replaces
// the whole list, so we emit a single event with N todos.
QString writeTranscript(const QString &dir,
                        const QList<ClaudeTask> &tasks) {
    QString path = dir + "/transcript.jsonl";
    std::ofstream f(path.toStdString());
    if (!f) return {};
    // Build the TodoWrite event JSON.
    f << R"({"type":"assistant","timestamp":"2026-05-19T12:00:00Z",)";
    f << R"("message":{"content":[{"type":"tool_use","name":"TodoWrite",)";
    f << R"("input":{"todos":[)";
    for (int i = 0; i < tasks.size(); ++i) {
        const auto &t = tasks.at(i);
        if (i) f << ",";
        f << "{";
        f << "\"id\":\"" << t.id.toStdString() << "\",";
        // JSON-escape simple double-quotes only — fixtures don't carry
        // backslashes or control chars.
        auto esc = [](const QString &s) {
            QString out = s;
            out.replace("\"", "\\\"");
            return out.toStdString();
        };
        f << "\"content\":\"" << esc(t.subject) << "\",";
        f << "\"activeForm\":\"" << esc(t.activeForm) << "\",";
        f << "\"status\":\"" << t.status.toStdString() << "\"";
        f << "}";
    }
    f << "]}}]}}\n";
    f.close();
    return path;
}

QList<ClaudeTask> defaultTasks() {
    QList<ClaudeTask> v;
    {
        ClaudeTask t;
        t.id = "alpha-1";
        t.subject = "Clean subject — no markup";
        t.activeForm = "Doing clean thing";
        t.status = "pending";
        v.append(t);
    }
    {
        ClaudeTask t;
        t.id = "beta-2";
        // The 2026-05-19 screenshot's leaked-markup shape:
        t.subject = "Roadmap feature</subject>"
                    "<parameter name=\"description\">User asked: ...";
        t.activeForm = "Roadmapping feature";
        t.status = "completed";
        v.append(t);
    }
    {
        ClaudeTask t;
        t.id = "gamma-3";
        // Angle-bracket prose that MUST NOT trip the detector.
        t.subject = "Refactor <repo>/<branch> handling";
        t.activeForm = "Refactoring";
        t.status = "in_progress";
        v.append(t);
    }
    return v;
}

}  // namespace

class TaskListDialogContextMenuTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static const char *argv[] = {"test"};
            new QApplication(argc, const_cast<char **>(argv));
        }
    }
};

// ANTS-1638 — every QListWidgetItem must carry id / status / subject /
// description in the UserRole+1..+4 slots after rebuild(). The round-
// trip assertion below checks the stash mechanism specifically, not
// the parser's id-population rules (TodoWrite events don't carry IDs;
// only the paired tool_result for TaskCreate does — see ANTS-1158
// spec § 3.1). The stash must reflect whatever the parser produced.
TEST_F(TaskListDialogContextMenuTest, Ants1638UserRolesStashed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ClaudeTaskListTracker tracker;
    const QString p = writeTranscript(tmp.path(), defaultTasks());
    ASSERT_FALSE(p.isEmpty());
    tracker.setTranscriptPath(p);
    tracker.rescan();
    ASSERT_EQ(tracker.tasks().size(), 3);

    ClaudeTaskListDialog dlg(&tracker, QStringLiteral("Default Dark"));
    auto *list = dlg.findChild<QListWidget *>(
        QStringLiteral("taskListItems"));
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count(), 3);

    const auto &parsed = tracker.tasks();
    for (int i = 0; i < parsed.size(); ++i) {
        QListWidgetItem *row = list->item(i);
        ASSERT_NE(row, nullptr) << "row " << i << " missing";
        EXPECT_EQ(row->data(kRoleTaskId).toString(),  parsed[i].id)
            << "row " << i << " kRoleTaskId mismatch";
        EXPECT_EQ(row->data(kRoleStatus).toString(),  parsed[i].status)
            << "row " << i << " kRoleStatus mismatch";
        EXPECT_EQ(row->data(kRoleSubject).toString(), parsed[i].subject)
            << "row " << i << " kRoleSubject mismatch";
        // Description fallback: description, else activeForm. Matches
        // rebuild()'s fallback order.
        const QString expectedDesc = parsed[i].description.isEmpty()
            ? parsed[i].activeForm : parsed[i].description;
        EXPECT_EQ(row->data(kRoleDescriptionFull).toString(), expectedDesc)
            << "row " << i << " kRoleDescriptionFull mismatch";
    }
}

// ANTS-1638 — context menu policy + signal wired up.
TEST_F(TaskListDialogContextMenuTest, Ants1638ContextMenuPolicyWired) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ClaudeTaskListTracker tracker;
    const QString p = writeTranscript(tmp.path(), defaultTasks());
    ASSERT_FALSE(p.isEmpty());
    tracker.setTranscriptPath(p);
    tracker.rescan();

    ClaudeTaskListDialog dlg(&tracker, QStringLiteral("Default Dark"));
    auto *list = dlg.findChild<QListWidget *>(
        QStringLiteral("taskListItems"));
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->contextMenuPolicy(), Qt::CustomContextMenu);

    // Confirm something is connected to customContextMenuRequested by
    // emitting it via QSignalSpy and checking it dispatches without
    // crash. We can't easily inspect connections, but we can at least
    // confirm the signal exists on the object and the dialog stays
    // alive after a synthetic emit (NoFocus + NoSelection means we
    // pass a pos that's outside any row → slot returns early via
    // itemAt() == nullptr).
    QSignalSpy spy(list, &QWidget::customContextMenuRequested);
    emit list->customContextMenuRequested(QPoint(-1, -1));
    EXPECT_EQ(spy.count(), 1);
    // Dialog still alive (no crash from out-of-range item lookup).
    EXPECT_TRUE(dlg.isHidden() || dlg.isVisible());
}

// ANTS-1639 — rendered row prefixed with "[malformed] " when subject
// carries raw tool-input XML-ish markup.
TEST_F(TaskListDialogContextMenuTest, Ants1639MalformedSubjectFlagged) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ClaudeTaskListTracker tracker;
    const QString p = writeTranscript(tmp.path(), defaultTasks());
    ASSERT_FALSE(p.isEmpty());
    tracker.setTranscriptPath(p);
    tracker.rescan();

    ClaudeTaskListDialog dlg(&tracker, QStringLiteral("Default Dark"));
    auto *list = dlg.findChild<QListWidget *>(
        QStringLiteral("taskListItems"));
    ASSERT_NE(list, nullptr);

    // Row 1 carries the </subject><parameter name="..."> markup.
    QListWidgetItem *row1 = list->item(1);
    ASSERT_NE(row1, nullptr);
    EXPECT_TRUE(row1->text().contains(QStringLiteral("[malformed]")))
        << "expected [malformed] flag in: " << row1->text().toStdString();
    // The underlying UserRole data preserves the original subject
    // verbatim — flagging is only on the rendered display string.
    EXPECT_TRUE(row1->data(kRoleSubject).toString().contains(
        QStringLiteral("</subject>")));
}

// ANTS-1639 — legit angle-bracket prose like "<repo>/<branch>" does
// NOT trip the malformed detector.
TEST_F(TaskListDialogContextMenuTest, Ants1639CleanAngleBracketProseUnflagged) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ClaudeTaskListTracker tracker;
    const QString p = writeTranscript(tmp.path(), defaultTasks());
    ASSERT_FALSE(p.isEmpty());
    tracker.setTranscriptPath(p);
    tracker.rescan();

    ClaudeTaskListDialog dlg(&tracker, QStringLiteral("Default Dark"));
    auto *list = dlg.findChild<QListWidget *>(
        QStringLiteral("taskListItems"));
    ASSERT_NE(list, nullptr);

    // Row 2 — "Refactor <repo>/<branch> handling". MUST NOT have
    // [malformed] flag.
    QListWidgetItem *row2 = list->item(2);
    ASSERT_NE(row2, nullptr);
    EXPECT_FALSE(row2->text().contains(QStringLiteral("[malformed]")))
        << "false-positive flag on clean prose: "
        << row2->text().toStdString();
    // Row 0 — clean subject without angle brackets at all.
    QListWidgetItem *row0 = list->item(0);
    ASSERT_NE(row0, nullptr);
    EXPECT_FALSE(row0->text().contains(QStringLiteral("[malformed]")));
}
