#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QFileSystemWatcher>

// One row in a Claude Code session's user-visible task list.
//
// The plan is sourced from the active session's JSONL transcript at
// `~/.claude/projects/<encoded-cwd>/<session-uuid>.jsonl`. Three event
// shapes carry plan state (see docs/specs/ANTS-1158.md §3.1):
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
    QString id;           // taskId (TaskCreate) or generated for TodoWrite items
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

// Per-session tracker mirroring ClaudeBgTaskTracker's shape:
//   * One QFileSystemWatcher on the focused tab's transcript path.
//   * fileChanged → rescan() → emit tasksChanged() iff the
//     id-or-status set changed.
//   * Pure static parser exposed for unit tests.
class ClaudeTaskListTracker : public QObject {
    Q_OBJECT

public:
    explicit ClaudeTaskListTracker(QObject *parent = nullptr);
    ~ClaudeTaskListTracker() override;

    // Set the active transcript JSONL path. Empty = inactive (no
    // tasks). Idempotent for the same path.
    void setTranscriptPath(const QString &path);

    const QString &transcriptPath() const { return m_transcriptPath; }
    const QList<ClaudeTask> &tasks() const { return m_tasks; }

    // Counts for the status-bar chip.
    int totalCount() const { return m_tasks.size(); }
    int unfinishedCount() const;     // ANTS-1221: pending only — running tasks are not user-actionable
    int inProgressCount() const;
    int pendingCount() const;
    int completedCount() const;

    // Pure parser — exposed for tests. Returns the plan list at the
    // tail of `path`. Stateless, GUI-thread-only (Qt I/O classes
    // aren't reentrant on the same handle).
    static QList<ClaudeTask> parseTranscript(const QString &path);

signals:
    void tasksChanged();

public slots:
    void rescan();
    // 2026-05-07 followup to ANTS-1158: polling rescue for the case
    // where QFileSystemWatcher silently drops its watch on atomic
    // rewrite. Claude Code writes the transcript via tmpfile+rename
    // on every TodoWrite/TaskCreate/TaskUpdate, which trips the
    // watcher and stops fileChanged from firing. Called from the
    // status-bar 2 s timer (refreshTasksButton). Cheap when settled:
    // an mtime check short-circuits the full JSONL parse.
    void poll();

public:
    // ANTS-1458 — diagnostic accessor for the chip-refresh
    // instrumentation (latency-investigation phase 1). Surfaces
    // the mtime that gated the last rescan so refreshTasksButton
    // can log (current_mtime, last_rescan_mtime, delta_ms) per
    // tick under ANTS_DEBUG=claude. Moved out of `public slots:`
    // (clazy const-signal-or-slot, 2026-05-19) — a const getter is
    // not a slot.
    qint64 lastRescanMtimeMs() const;

private:
    QString m_transcriptPath;
    QFileSystemWatcher m_watcher;
    QList<ClaudeTask> m_tasks;
    qint64 m_lastRescanMtimeMs = 0;
};
