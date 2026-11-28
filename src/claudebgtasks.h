#pragma once

#include <QString>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>

#include "claudetranscripttracker.h"
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
    QString id;             // Shell: backgroundTaskId. Agent: agentId. Monitor: taskId.
    QString tool;           // the launching tool's name: "Bash", "Agent", "Monitor"…
    QString description;    // input.description, else input.subagent_type, else `command`
    QString command;        // the call's command or prompt — truncated by display
    QString outputPath;     // Shell: from the result text. Agent: outputFile. Monitor: empty.
    QDateTime startedAt;    // assistant event timestamp, or the latest resume's
    bool finished = false;  // a completion or kill event was seen, and no resume since
    // ANTS-1840 — true only when `finished` was inferred from the output
    // file being gone, NOT by an authoritative transcript completion/kill
    // event. sweepLiveness un-latches it if the file reappears; a
    // transcript-finish is permanent.
    bool finishedByLiveness = false;
    // ANTS-6326 — an Agent's meta file holds finished:true as of the last read.
    // True at a pause as well as at the end; see ClaudeBgTaskTracker::finalize.
    bool helperStopped = false;
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
struct ClaudeBgRunningCounts {
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
    // ANTS-6307 — ids ended before the tool_result that names them. A job
    // that ends at once has its notice written ahead of its launch result.
    QSet<QString>               endedBeforeStart;
    // ANTS-6321 — helper transcript path → how far it has been read.
    QHash<QString, ClaudeTranscript::Cursor> helperCursors;
};

// Per-session tracker for Claude Code background tasks. Binding, watching
// and rescanning come from ClaudeTranscriptTracker; tasksChanged() fires when
// a task's id or finished flag changed. poll() also notices a helper agent's
// transcript moving (ANTS-6321).
class ClaudeBgTaskTracker
    : public ClaudeTranscriptTrackerOf<ClaudeBackgroundTask, ClaudeBgTaskAccum> {
    Q_OBJECT

public:
    explicit ClaudeBgTaskTracker(QObject *parent = nullptr);

    int runningCount() const;

    // Pure parser — exposed for tests. Returns all background tasks
    // observed in the transcript at `path`, with `finished` set when a
    // matching completion or kill event appears later in the same
    // transcript. Stateless and safe to call concurrently: each call opens
    // its own file handle.
    static QList<ClaudeBackgroundTask> parseTranscript(const QString &path);

    // ANTS-5050 — parse only what has been appended since `cursor`, folding
    // it into `acc`, and return the tracker's visible list. The caller must
    // call ClaudeTranscript::resetIfStale on `cursor` and `acc` first.
    //
    // ANTS-5773 — a task whose startedAt precedes processStartMs is not
    // running: it belonged to an earlier Claude process. 0 = unknown; the
    // rule is skipped.
    //
    // ANTS-6321 — also folds in the session's helper transcripts.
    static QList<ClaudeBackgroundTask> parseIncremental(
        const QString &path, ClaudeTranscript::Cursor &cursor,
        ClaudeBgTaskAccum &acc, qint64 processStartMs = 0);
    // ANTS-6321 — fold what `path`'s helpers, <session>/subagents/agent-*.jsonl,
    // have appended into `acc`, and end a helper whose meta file says finished
    // (ANTS-6322). True if anything moved. A helper can write while the main
    // transcript is silent, so the 2 s polls call this too.
    static bool walkHelperTranscripts(const QString &path, ClaudeBgTaskAccum &acc);
    // The end-of-parse filters over an accumulator, re-runnable without a
    // walk; applies the processStartMs rule above.
    static QList<ClaudeBackgroundTask> finalize(const ClaudeBgTaskAccum &acc,
                                                qint64 processStartMs = 0);
    static ClaudeBgRunningCounts runningCounts(const QList<ClaudeBackgroundTask> &tasks);

    // The start time of the Claude process this tracker's transcript belongs
    // to, passed to parseIncremental and finalize. 0 = unknown.
    void setProcessStartMs(qint64 ms);

public slots:
    // 0.7.55 (2026-04-27 indie-review) — lightweight liveness sweep,
    // separate from rescan(). Walks m_tasks only and updates the
    // `finished` flag based on whether each task's output file exists. NO
    // transcript reparse — costs N stat() calls instead of a
    // transcript walk. Called from the status-bar 2 s timer
    // (ClaudeStatusBarController::refreshBgTasksButton); rescan() continues to
    // run on actual transcript changes.
    void sweepLiveness();

protected:
    QList<ClaudeBackgroundTask> parse(const QString &path,
                                      ClaudeTranscript::Cursor &cursor,
                                      ClaudeBgTaskAccum &acc) const override;
    bool sameRow(const ClaudeBackgroundTask &a,
                 const ClaudeBackgroundTask &b) const override;
    // ANTS-6321 — a helper agent's transcript.
    bool otherSourcesMoved() override;

private:
    qint64 m_processStartMs = 0;  // ANTS-5773
};
