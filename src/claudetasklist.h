#pragma once

#include <QString>
#include <QHash>
#include <QList>

#include "claudetranscripttracker.h"
#include "claudetranscriptwalker.h"

// One row in a Claude Code session's user-visible task list.
//
// The plan is sourced from the active session's JSONL transcript at
// `~/.claude/projects/<encoded-cwd>/<session-uuid>.jsonl`. Three event
// shapes carry plan state (see docs/specs/ANTS-1158-claude-code-task-list-status.md §3.1):
//
//   * TodoWrite — full snapshot, replaces the list. (Most-recent wins.)
//   * TaskCreate — incremental add; ID arrives in the paired
//     tool_result.
//   * TaskUpdate — flips status on an existing entry by taskId.
//
// Filters applied:
//   * Events with `isSidechain == true` are ignored (subagent turns
//     inline their tool calls in the parent transcript with that flag).
//   * `Task` tool_use with `subagent_type` is a subagent dispatch, not
//     a plan-list-add — filtered out.
struct ClaudeTask {
    QString id;           // taskId (TaskCreate); empty for TodoWrite items
    QString subject;      // user-visible title
    QString description;  // longer text (optional)
    QString activeForm;   // present-continuous form (optional)
    QString status;       // "pending" / "in_progress" / "completed" / "deleted"
                          // (ANTS-1407: `deleted` is filtered out at end of
                          // parseTranscript — never user-visible, but lives
                          // mid-walk so TaskUpdate-by-id can find the entry.)
    qint64  lastEventAtMs = 0;  // ANTS-1341: epoch ms of the latest event
                                // affecting this task (TaskCreate timestamp;
                                // bumped on each TaskUpdate; TodoWrite sets
                                // it from the snapshot's event timestamp).
                                // Zero if absent or unparseable — fail-soft
                                // preserves the task (abandonment INV-7).
};

// ANTS-5050 — the parse state that survives between incremental walks.
//
// `raw` is the list BEFORE the end-of-parse filters: `deleted` entries live
// here so a later TaskUpdate-by-id can still find them (ANTS-1407), and the
// abandonment filter is re-applied on each emit rather than baked in. So the
// finalize step is pure and re-runnable, which is what lets a resumed walk
// produce the same answer as a cold one.
struct ClaudeTaskAccum {
    QList<ClaudeTask>   raw;
    bool                sawTodoWrite = false;
    QHash<QString, int> idxByToolUseId;  // tool_use_id → index into `raw`
};

// Per-session tracker of the focused tab's task list. Binding, watching and
// rescanning come from ClaudeTranscriptTracker; tasksChanged() fires when a
// row's id, status or subject changed. Pure static parser exposed for unit
// tests.
class ClaudeTaskListTracker
    : public ClaudeTranscriptTrackerOf<ClaudeTask, ClaudeTaskAccum> {
    Q_OBJECT

public:
    explicit ClaudeTaskListTracker(QObject *parent = nullptr);

    // Counts for the status-bar chip.
    int totalCount() const { return m_tasks.size(); }
    int unfinishedCount() const;     // ANTS-1221: pending only — running tasks are not user-actionable
    int inProgressCount() const;
    int pendingCount() const;
    int completedCount() const;

    // Pure parser — exposed for tests. Returns the plan list at the
    // tail of `path`. Stateless and safe to call concurrently: each
    // call opens its own file handle.
    static QList<ClaudeTask> parseTranscript(const QString &path);

    // ANTS-5050 — parse only what has been appended since `cursor`, folding
    // it into `acc`, and return the tracker's visible list.
    //
    // The caller must call ClaudeTranscript::canResume first and, on false,
    // reset BOTH `cursor` and `acc` — a cursor without its accumulator
    // resumes into a half-built list. `parseTranscript` above is this with a
    // fresh cursor and accumulator, and remains the full-parse contract.
    static QList<ClaudeTask> parseIncremental(const QString &path,
                                              ClaudeTranscript::Cursor &cursor,
                                              ClaudeTaskAccum &acc);

protected:
    QList<ClaudeTask> parse(const QString &path, ClaudeTranscript::Cursor &cursor,
                            ClaudeTaskAccum &acc) const override;
    bool sameRow(const ClaudeTask &a, const ClaudeTask &b) const override;
};
