#include "treewatcher.h"

#include <QSocketNotifier>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSet>

#include <sys/inotify.h>
#include <unistd.h>

namespace {
// Directory-watch mask: content edits of children (IN_MODIFY), new/removed
// entries (IN_CREATE/IN_DELETE), renames (IN_MOVED_FROM/TO), and the watched
// dir itself vanishing (IN_DELETE_SELF/IN_MOVE_SELF). IN_MODIFY on a directory
// watch is the load-bearing one — it is what QFileSystemWatcher drops.
constexpr uint32_t kMask =
    IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_DELETE_SELF | IN_MOVE_SELF;
}  // namespace

DirTreeWatcher::DirTreeWatcher(QObject *parent) : QObject(parent) {
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd >= 0) {
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated,
                this, &DirTreeWatcher::onActivated);
    }
    // Coalesce a burst (a `git pull` or a multi-file save) into one changed().
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(300);
    connect(m_debounce, &QTimer::timeout, this, &DirTreeWatcher::changed);
}

DirTreeWatcher::~DirTreeWatcher() {
    if (m_notifier) m_notifier->setEnabled(false);
    if (m_fd >= 0) ::close(m_fd);   // closing the fd releases every watch
}

int DirTreeWatcher::addDirs(const QStringList &dirs) {
    for (const QString &d : dirs) addOne(d);
    return m_wdToPath.size();
}

void DirTreeWatcher::addOne(const QString &dir) {
    if (m_fd < 0) return;
    if (m_pathToWd.contains(dir)) return;           // already watched
    if (m_wdToPath.size() >= m_maxWatches) return;  // safety cap — degrade
    if (!QFileInfo(dir).isDir()) return;
    const int wd = inotify_add_watch(m_fd, QFile::encodeName(dir).constData(),
                                     kMask);
    if (wd < 0) return;
    m_wdToPath.insert(wd, dir);
    m_pathToWd.insert(dir, wd);
}

void DirTreeWatcher::onActivated() {
    if (m_fd < 0) return;
    // inotify delivers a packed buffer of variable-length records; drain
    // everything currently available (the fd is non-blocking → EAGAIN ends it).
    alignas(struct inotify_event) char buf[64 * 1024];
    bool any = false;
    for (;;) {
        const ssize_t n = ::read(m_fd, buf, sizeof(buf));
        if (n <= 0) break;   // EAGAIN (drained) or error
        any = true;
        for (char *p = buf; p < buf + n; ) {
            auto *ev = reinterpret_cast<struct inotify_event *>(p);
            // A watched directory was removed / auto-invalidated: drop its wd
            // so watchCount stays honest and a re-seed can re-add it.
            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                const QString gone = m_wdToPath.take(ev->wd);
                if (!gone.isEmpty()) m_pathToWd.remove(gone);
            }
            // IN_Q_OVERFLOW (kernel dropped events) needs no special handling
            // beyond firing changed() below — a full re-probe recovers.
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    if (any) m_debounce->start();
}

QStringList DirTreeWatcher::directoriesContaining(const QString &topLevel,
                                                  const QByteArray &lsFilesZeroSep) {
    QSet<QString> dirs;
    const QString root = topLevel.isEmpty()
        ? QString() : QDir::cleanPath(topLevel);
    const QList<QByteArray> rels = lsFilesZeroSep.split('\0');
    for (const QByteArray &rel : rels) {
        if (rel.isEmpty()) continue;
        const QString relPath = QString::fromUtf8(rel);
        const QString abs = root.isEmpty()
            ? relPath : root + QLatin1Char('/') + relPath;
        dirs.insert(QFileInfo(abs).path());   // the file's directory
    }
    QStringList out(dirs.begin(), dirs.end());
    out.sort();
    return out;
}
