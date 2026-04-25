#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QList>
#include <QFileSystemWatcher>

// One Claude Code background task, derived from a transcript scan.
//
// Background tasks are launched by the assistant invoking `Bash` or `Task`
// with `input.run_in_background: true`. Claude Code records the launch as
// an `assistant` event whose `message.content[]` contains a `tool_use`
// block with that flag, and the immediate user-side `tool_result` carries
// `toolUseResult.backgroundTaskId` plus a path of the form
//   /tmp/claude-$UID/<encoded-project>/<session-uuid>/tasks/<id>.output
// to which stdout/stderr is streamed.
//
// A task is considered finished when the transcript later contains a
// BashOutput tool_result whose `toolUseResult.status == "completed"` for
// the same backgroundTaskId, or a KillShell tool_use referencing the same
// id, or when the session is replayed and the originating session is no
// longer the most recent one for the project. In MVP scope we treat the
// presence of a "completed"/"killed" event in the same transcript as the
// only finish signal — out-of-band kills (Claude Code crash, /tmp purge)
// will simply leave the task in `finished == false` until the next
// rescan after the user reopens the project, which is acceptable.
struct ClaudeBackgroundTask {
    QString id;             // backgroundTaskId, matches <id>.output basename stem
    QString tool;           // "Bash" / "Task" / etc.
    QString description;    // input.description (Bash) or input.subagent_type (Task)
    QString command;        // input.command or input.prompt — truncated by display
    QString outputPath;     // /tmp/claude-$UID/.../<id>.output
    QDateTime startedAt;    // assistant event timestamp
    bool finished = false;  // a completion or kill event was seen for this id
    int exitCode = -1;      // captured from completion event when present
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

    QString transcriptPath() const { return m_transcriptPath; }
    QList<ClaudeBackgroundTask> tasks() const { return m_tasks; }
    int runningCount() const;

    // Pure parser — exposed for tests. Returns all background tasks
    // observed in the transcript at `path`, with `finished` set when a
    // matching completion or kill event appears later in the same
    // transcript. Stateless and safe to call concurrently.
    static QList<ClaudeBackgroundTask> parseTranscript(const QString &path);

signals:
    void tasksChanged();

private slots:
    void rescan();

private:
    QString m_transcriptPath;
    QFileSystemWatcher m_watcher;
    QList<ClaudeBackgroundTask> m_tasks;
};
