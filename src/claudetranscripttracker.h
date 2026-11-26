#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <utility>

#include "claudetranscriptwalker.h"

// ANTS-6228 — what ClaudeBgTaskTracker and ClaudeTaskListTracker share:
// binding a transcript path, watching it through the shared
// ClaudeTranscriptWatch, the debounced rescan, and poll()'s rescue for a
// dropped watch. A tracker supplies only its per-event fold, through
// ClaudeTranscriptTrackerOf below.
class ClaudeTranscriptTracker : public QObject {
    Q_OBJECT

public:
    ~ClaudeTranscriptTracker() override;

    // Set the active transcript JSONL path. Empty = inactive (no tasks).
    // Idempotent for the same path.
    void setTranscriptPath(const QString &path);
    const QString &transcriptPath() const { return m_transcriptPath; }

    // ANTS-1458 — the mtime that gated the last rescan, for the chip-refresh
    // log under ANTS_DEBUG=claude.
    qint64 lastRescanMtimeMs() const { return m_lastRescanMtimeMs; }

signals:
    void tasksChanged();

public slots:
    // Fold in what was appended to the transcript and replace the task list;
    // emits tasksChanged() only when the list's shape changed. Driven by the
    // shared watch (debounced) and by poll().
    void rescan();

    // QFileSystemWatcher silently drops its watch when a file is replaced by
    // tmpfile+rename, so the status bar's 2 s timer calls this. It re-adds a
    // lost watch, and rescans only when the transcript's mtime or size moved
    // since the last rescan, or a tracker's other source did.
    void poll();

protected:
    explicit ClaudeTranscriptTracker(QObject *parent);

    // The path changed: drop the old path's walk state and take what is kept
    // for the new one.
    virtual void rebindWalkState() = 0;
    // Fold what was appended into the walk state and replace the task list.
    // True when the list's shape changed.
    virtual bool refold() = 0;
    // poll() asks this only when the transcript itself has not moved: true
    // when something else the tracker reads has.
    virtual bool otherSourcesMoved() { return false; }

private:
    QString m_transcriptPath;

    // ANTS-5050 — coalesce the fileChanged storm. Claude appends to the
    // transcript continuously while it works, and each append fires the
    // watch, so a burst costs one walk instead of one per append.
    // Single-shot and restarted per signal, so a steady stream settles into
    // one walk per interval rather than starving. setTranscriptPath still
    // rescans at once.
    QTimer m_rescanDebounce;
    static constexpr int kRescanDebounceMs = 250;

    // ANTS-1458 phase 2 — the change signal sampled before the last rescan's
    // read. Size is the second signal, so a same-millisecond append that
    // left mtime unchanged still triggers a rescan.
    qint64 m_lastRescanMtimeMs = 0;
    qint64 m_lastRescanSizeBytes = 0;
};

// ANTS-6228 — the walk state for one kind of task: the resumable cursor, its
// accumulator, the per-path cache every tracker of the kind shares, and the
// visible task list. A tracker supplies parse() and sameRow().
template <typename Task, typename Accum>
class ClaudeTranscriptTrackerOf : public ClaudeTranscriptTracker {
public:
    // Returned by reference: the 2 s status timer reads it on every tick.
    const QList<Task> &tasks() const { return m_tasks; }

protected:
    using ClaudeTranscriptTracker::ClaudeTranscriptTracker;

    // Fold what `path` appended since `cursor` into `acc`; return the
    // visible list.
    virtual QList<Task> parse(const QString &path, ClaudeTranscript::Cursor &cursor,
                              Accum &acc) const = 0;
    // Whether two rows look the same to the UI. tasksChanged() is skipped
    // when every row does, so a metadata-only append causes no repaint.
    virtual bool sameRow(const Task &a, const Task &b) const = 0;

    // Replace the task list; true when its shape changed.
    bool replaceTasks(QList<Task> next) {
        bool same = next.size() == m_tasks.size();
        for (int i = 0; same && i < next.size(); ++i)
            same = sameRow(next.at(i), m_tasks.at(i));
        m_tasks = std::move(next);
        return !same;
    }

    // ANTS-5050 INV-6 — the cursor and accumulator belong to the OLD path;
    // resuming either against another file would fold one session's events
    // into another's list. INV-9 — take the state kept for the NEW path, so
    // a tab-switch re-bind resumes rather than cold-walking.
    void rebindWalkState() override {
        m_cursor = {};
        m_acc = {};
        walkCache().restore(transcriptPath(), m_cursor, m_acc);
    }

    // ANTS-5050 — parse only what was appended since the last rescan. The
    // cursor and accumulator reset together whenever the cursor cannot be
    // trusted (truncated, replaced, rewritten); see ClaudeTranscript::canResume.
    bool refold() override {
        QList<Task> next;
        const QString &path = transcriptPath();
        if (!path.isEmpty()) {
            ClaudeTranscript::resetIfStale(path, m_cursor, m_acc);
            next = parse(path, m_cursor, m_acc);
            walkCache().store(path, m_cursor, m_acc);  // INV-9
        }
        return replaceTasks(std::move(next));
    }

    QList<Task> m_tasks;
    // Replaced together, and only together.
    ClaudeTranscript::Cursor m_cursor;
    Accum m_acc;

private:
    // ANTS-5050 INV-9 — one cache per kind of task, so panes sharing a
    // transcript share its state.
    static ClaudeTranscript::WalkCache<Accum> &walkCache() {
        static ClaudeTranscript::WalkCache<Accum> cache;
        return cache;
    }
};
