#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QTimer>

#include "claudetranscriptwalker.h"

// One Claude Code background task, derived from a transcript scan: a
// background shell, an async agent or a monitor. The records that start and
// end one are listed above applyBgEvent in claudebgtasks.cpp (ANTS-5773
// spec § 2.1). A shell streams its output to
//   /tmp/claude-$UID/<encoded-project>/<session-uuid>/tasks/<id>.output
// and a task whose output file is gone counts as finished.
struct ClaudeBackgroundTask {
    // ANTS-5773 — what kind of background work this is; the per-tab counts
    // are taken by it.
    enum class Kind { Shell, Agent, Monitor };
    QString id;             // backgroundTaskId, matches <id>.output basename stem
    QString tool;           // "Bash" / "Task" / etc.
    QString description;    // input.description (Bash) or input.subagent_type (Task)
    QString command;        // input.command or input.prompt — truncated by display
    QString outputPath;     // /tmp/claude-$UID/.../<id>.output
    QDateTime startedAt;    // assistant event timestamp
    bool finished = false;  // a completion or kill event was seen for this id
    // ANTS-1840 — true only when `finished` was inferred from the output
    // file being gone, NOT by an authoritative transcript completion/kill
    // event. sweepLiveness un-latches it if the file reappears; a
    // transcript-finish is permanent.
    bool finishedByLiveness = false;
    Kind kind = Kind::Shell;
};

// ANTS-5773 — a tool_use that may start a background task, remembered until
// its tool_result arrives.
struct ClaudeBgPendingCall {
    QString name;
    QString description;
    QString command;
    QDateTime startedAt;
};

// ANTS-5773 — running tasks by kind.
struct ClaudeBgPendingCounts {
    int shells = 0;
    int agents = 0;
    int monitors = 0;
};

// ANTS-5050 — the parse state that survives between incremental walks.
//
// `raw` is the list BEFORE the end-of-parse filters: a launch with no
// backgroundTaskId yet stays here, because the id arrives in a LATER event
// and dropping it at accumulate time would lose the entry the confirmation
// refers to. The liveness sweep is wall-clock dependent, so it is re-applied
// on every emit rather than baked into `raw`.
struct ClaudeBgTaskAccum {
    QList<ClaudeBackgroundTask> raw;
    QHash<QString, int>         idxByToolUseId;  // tool_use_id → index
    QHash<QString, int>         idxByBgId;       // backgroundTaskId → index
    QHash<QString, ClaudeBgPendingCall> calls;   // ANTS-5773 — tool_use_id → call
};

// Per-session tracker for Claude Code background tasks.
//
// Owns a QFileSystemWatcher on the active transcript path and re-parses
// on change. Emits `tasksChanged()` when the running-count or the task
// list shape changes.

class ClaudeBgTaskTracker : public QObject {
    Q_OBJECT

public:
    explicit ClaudeBgTaskTracker(QObject *parent = nullptr);
    ~ClaudeBgTaskTracker() override;

    // Set the active transcript JSONL path. Empty = inactive (no tasks).
    // Idempotent for the same path.
    void setTranscriptPath(const QString &path);

    const QString &transcriptPath() const { return m_transcriptPath; }
    // 0.7.55 (2026-04-27 indie-review — cppcheck returnByReference) —
    // hot-path read called from the 2 s status-timer; returning by
    // value copied the whole vector every tick. Caller doesn't mutate.
    const QList<ClaudeBackgroundTask> &tasks() const { return m_tasks; }
    int runningCount() const;

    // Pure parser — exposed for tests. Returns all background tasks
    // observed in the transcript at `path`, with `finished` set when a
    // matching completion or kill event appears later in the same
    // transcript. Stateless and safe to call concurrently.
    static QList<ClaudeBackgroundTask> parseTranscript(const QString &path);

    // ANTS-5050 — parse only what has been appended since `cursor`, folding
    // it into `acc`, and return the tracker's visible list. The caller must
    // call ClaudeTranscript::canResume first and, on false, reset BOTH
    // `cursor` and `acc`.
    //
    // ANTS-5773 — a task whose startedAt precedes processStartMs is not
    // running: it belonged to an earlier Claude process. 0 = unknown; the
    // rule is skipped.
    static QList<ClaudeBackgroundTask> parseIncremental(
        const QString &path, ClaudeTranscript::Cursor &cursor,
        ClaudeBgTaskAccum &acc, qint64 processStartMs = 0);
    // The end-of-parse filters over an accumulator, re-runnable without a
    // walk; applies the processStartMs rule above.
    static QList<ClaudeBackgroundTask> finalize(const ClaudeBgTaskAccum &acc,
                                                qint64 processStartMs = 0);
    static ClaudeBgPendingCounts pendingCounts(const QList<ClaudeBackgroundTask> &tasks);

    // The start time of the Claude process this tracker's transcript belongs
    // to, passed to parseIncremental and finalize. 0 = unknown.
    void setProcessStartMs(qint64 ms);

signals:
    void tasksChanged();

public slots:
    // Re-parse the current transcript and update the task list. Driven
    // by the QFileSystemWatcher on transcript-changed (write-append by
    // Claude Code). One transcript walk capped at 16 MiB.
    void rescan();

    // 0.7.55 (2026-04-27 indie-review) — lightweight liveness sweep,
    // separate from rescan(). Walks m_tasks only and updates the
    // `finished` flag based on whether each task's output file exists. NO
    // transcript reparse — costs N stat() calls instead of a 16 MiB
    // file read. Called from the status-bar 2 s timer
    // (mainwindow.cpp::refreshBgTasksButton); rescan() continues to
    // run on actual transcript changes. Was previously rescan() on
    // every tick — this slot replaces that hot path.
    void sweepLiveness();

    // Watch-loss recovery mirror of ClaudeTaskListTracker::poll(). Qt's
    // QFileSystemWatcher silently drops the path on tmpfile+rename, so
    // the 2 s status-timer cycles a cheap mtime check; reparses only
    // when mtime advanced. Also re-adds the watcher when the file has
    // (re-)appeared. Cheap on the steady-state no-change case.
    void poll();

private:
    QString m_transcriptPath;
    // ANTS-1285 — the transcript is watched through the shared
    // ClaudeTranscriptWatch, which this tracker subscribes to per path.

    // ANTS-5050 — coalesce the fileChanged storm. Claude appends to the
    // transcript continuously while it works, and each signal drove a
    // full rescan, which walks up to a 16 MiB tail and JSON-parses every
    // line. The watcher fires far faster than that walk is worth
    // repeating, so a burst of appends now costs one walk instead of one
    // per append. Single-shot and restarted per signal, so a steady
    // stream settles into one walk per interval rather than starving.
    // The poll() mtime/size shortcut is unaffected, and
    // setTranscriptPath still rescans immediately rather than waiting.
    QTimer m_rescanDebounce;
    static constexpr int kRescanDebounceMs = 250;

    QList<ClaudeBackgroundTask> m_tasks;

    // ANTS-5050 — the resumable parse position and its accumulated state.
    // Replaced together, and only together.
    ClaudeTranscript::Cursor m_cursor;
    ClaudeBgTaskAccum m_acc;

    // ANTS-5050 INV-9 — kept walk state per transcript path, shared by every
    // background-task tracker.
    static ClaudeTranscript::WalkCache<ClaudeBgTaskAccum> &walkCache();

    qint64 m_processStartMs = 0;  // ANTS-5773
    qint64 m_lastRescanMtimeMs = 0;
    // ANTS-1458 phase 2 — mirror ClaudeTaskListTracker: size is a second
    // change-signal so poll() re-parses on a same-mtime append.
    qint64 m_lastRescanSizeBytes = 0;
};
