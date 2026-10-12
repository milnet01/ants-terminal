#pragma once

// ANTS-1285 — one QFileSystemWatcher for every transcript tracker.
//
// ClaudeTaskListTracker and ClaudeBgTaskTracker are bound to the same
// focused-tab transcript. Each used to own a watcher, so one file carried two
// inotify watches and every append woke both. The hub holds one watch per
// path, counted by subscriber, and broadcasts `changed(path)`; each tracker
// filters on its own path.
//
// QFileSystemWatcher drops a path when the file is replaced by an atomic
// rename. `ensure()` re-adds it, so the callers' existing re-watch points
// (rescan, poll) keep working against the shared watch.
//
// GUI thread only, like the trackers it serves. The hub is a child of the
// QCoreApplication, so it never outlives the event loop its watcher needs;
// a subscriber destroyed after the application uses existing().

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QString>

class ClaudeTranscriptWatch : public QObject {
    Q_OBJECT
public:
    static ClaudeTranscriptWatch &instance();
    // The hub if it has been created and not yet destroyed, else nullptr.
    static ClaudeTranscriptWatch *existing();

    // Count one subscriber on `path`; watch it if the file exists.
    void acquire(const QString &path);
    // Drop one subscriber; the watch goes when the last one does.
    void release(const QString &path);
    // Re-add a subscribed path the watcher has dropped, when the file exists.
    // Returns true when it had to re-add, i.e. a change may have been missed.
    bool ensure(const QString &path);

    bool isWatching(const QString &path) const;
    int subscribers(const QString &path) const;

signals:
    void changed(const QString &path);

private:
    explicit ClaudeTranscriptWatch(QObject *parent);

    QFileSystemWatcher m_watcher;
    QHash<QString, int> m_refs;
};
