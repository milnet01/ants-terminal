// treewatcher — a lightweight directory-tree change watcher over raw Linux
// inotify (ANTS-3509).
//
// Why not QFileSystemWatcher: Qt's *directory* watch does NOT surface a
// content EDIT of a child file — it filters out inotify's IN_MODIFY on
// children and only emits directoryChanged for add/remove/rename. So an
// editor or agent modifying an existing tracked file fired nothing, and the
// Review Changes dialog went stale until a commit or a manual Refresh
// (characterized headlessly 2026-07-13). Raw inotify on a directory watch
// DOES report IN_MODIFY for a direct child, so we can watch *directories only*
// (≈4× fewer watches than one file-watch per tracked file) and still catch
// edits.
//
// Linux-only (inotify) — the project is Linux-only. One inotify fd + one
// QSocketNotifier drive the whole tree; each watch is ~1 KB of kernel memory
// and every watch is released when the object is destroyed (closing the fd
// drops them all), so a transient dialog costs nothing once closed.
//
// The watcher is deliberately dumb: it watches exactly the directories it is
// handed (each non-recursively) and debounces every create/modify/delete/move
// into a single changed() signal. It does NOT auto-descend into new
// sub-directories — the caller re-seeds its (gitignore-aware) directory set on
// changed(), so ignored trees like build/ never get watched. Keeping the
// gitignore knowledge in the caller keeps this component a pure primitive.
#pragma once

#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>

class QSocketNotifier;
class QTimer;

class DirTreeWatcher : public QObject {
    Q_OBJECT
public:
    explicit DirTreeWatcher(QObject *parent = nullptr);
    ~DirTreeWatcher() override;

    // Watch each directory in `dirs` (non-recursively). Idempotent —
    // already-watched dirs and non-directories are skipped, so this is the
    // top-up call the caller re-issues with a fresh gitignore-aware set on
    // changed(). Silently stops adding once `maxWatches` is reached (degrade,
    // never fail). Returns the number of directories now watched.
    int addDirs(const QStringList &dirs);

    // Number of live inotify watches.
    int watchCount() const { return m_wdToPath.size(); }

    // False when inotify_init failed (no fd) — the caller may fall back.
    bool ok() const { return m_fd >= 0; }

    // Pure helper (static, testable): given the NUL-separated output of
    // `git ls-files -z …` and the working-tree root, return the sorted,
    // de-duplicated set of absolute directories that contain those files.
    // This is the gitignore-aware directory set a git consumer watches.
    static QStringList directoriesContaining(const QString &topLevel,
                                             const QByteArray &lsFilesZeroSep);

signals:
    // Emitted (debounced) when any watched directory sees a
    // create/modify/delete/move, or the kernel event queue overflows. The
    // consumer re-probes (and, for a git tree, re-seeds the dir set).
    void changed();

private:
    void onActivated();
    void addOne(const QString &dir);

    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_debounce = nullptr;
    QHash<int, QString> m_wdToPath;   // inotify watch-descriptor → dir path
    QHash<QString, int> m_pathToWd;   // reverse, for O(1) de-dup
    int m_maxWatches = 20000;         // safety cap (≈20 MB kernel worst case;
                                      // fs.inotify.max_user_watches is 65 k+)
};
