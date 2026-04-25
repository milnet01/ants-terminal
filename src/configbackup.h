#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

// Persistence hygiene helpers shared across Config, ClaudeAllowlist,
// SessionManager, and SettingsDialog. Split out of secureio.h in 0.7.31
// once the file straddled two concerns: 0600 perms (kept in secureio.h)
// and silent-data-loss / write-coordination (this file).

// Rotate a corrupt JSON-ish file aside so the next save() doesn't
// clobber it. Used by Config, Claude allowlist, SessionManager, and
// SettingsDialog — the 0.7.12 /indie-review sweep flagged the same
// silent-data-loss pattern (parse failure → next save overwrites user
// bytes) in all four, and the reviewers asked for a shared helper
// rather than four copies of the same ~30-line block.
//
// Behavior:
//   - Builds `<path>.corrupt-<ms_timestamp>` and tries QFile::copy.
//   - On collision (QFile::copy refuses to overwrite) retries with
//     `-1`, `-2`, ... up to 10 suffixes. Races beyond that are
//     pathological (same-millisecond multi-launch) and fall through
//     to the failure path.
//   - On success, prunes older siblings: any `<basename>.corrupt-*`
//     files beyond the newest 5 are removed. Keeps recovery material
//     available while preventing unbounded growth when a user keeps
//     launching Ants with a broken config.
//   - Returns the chosen backup path on success, empty QString on
//     failure (disk full, perms, 10 ms collisions).
//
// Callers log success/failure using their own DebugLog category.
inline QString rotateCorruptFileAside(const QString &path) {
    const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    QString chosen;
    for (int attempt = 0; attempt < 10; ++attempt) {
        QString candidate = path + QStringLiteral(".corrupt-")
                          + QString::number(stamp);
        if (attempt > 0) {
            candidate += QStringLiteral("-") + QString::number(attempt);
        }
        if (QFile::copy(path, candidate)) {
            chosen = candidate;
            break;
        }
    }
    if (chosen.isEmpty()) return QString();

    // Prune older siblings beyond the newest 5. Siblings live in the
    // same directory and share the `<basename>.corrupt-*` prefix.
    const QFileInfo info(path);
    QDir dir = info.absoluteDir();
    const QString prefix = info.fileName() + QStringLiteral(".corrupt-");
    QFileInfoList sibs = dir.entryInfoList(
        {prefix + QStringLiteral("*")},
        QDir::Files | QDir::NoSymLinks,
        QDir::Time);  // newest first
    constexpr int kKeep = 5;
    for (int i = kKeep; i < sibs.size(); ++i) {
        QFile::remove(sibs.at(i).absoluteFilePath());
    }
    return chosen;
}

// Cooperative inter-process write lock for shared config files.
//
// Two Ants processes that both call Config::save() at the same time
// race over `<path>.tmp` + `rename()`: both truncate the same tmp
// file, partial bytes from one writer can rename over the other, and
// last-rename-wins silently destroys whichever writer lost. Same risk
// for `~/.claude/settings.json` — Ants Allowlist + Ants Install-hook
// + concurrent Claude Code writes all converge on one path.
//
// `ConfigWriteLock` takes an advisory POSIX flock(2) on a sibling
// `<path>.lock` file. Lock file persists between runs (cheap empty
// file, 0600). RAII: scope-bound, releases on destruction.
//
// Behavior:
//   - Polls flock(2) with LOCK_EX | LOCK_NB up to 100 × 50 ms = 5 s.
//   - On timeout, `acquired()` returns false; caller decides whether
//     to skip the save or proceed unprotected (prefer skip).
//   - flock is *advisory* — only cooperating processes that also call
//     this helper are serialised. That covers the in-tree call sites;
//     external editors (vim, jq, sed -i) bypass the lock by design.
class ConfigWriteLock {
public:
    explicit ConfigWriteLock(const QString &targetPath) {
        const QString lockPath = targetPath + QStringLiteral(".lock");
        m_fd = ::open(lockPath.toLocal8Bit().constData(),
                      O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_fd < 0) return;
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 50 * 1000 * 1000;  // 50 ms
        for (int i = 0; i < 100; ++i) {  // 100 × 50ms = 5 s deadline
            if (::flock(m_fd, LOCK_EX | LOCK_NB) == 0) {
                m_held = true;
                return;
            }
            if (errno != EWOULDBLOCK) break;
            nanosleep(&ts, nullptr);
        }
        ::close(m_fd);
        m_fd = -1;
    }

    ~ConfigWriteLock() {
        if (m_fd >= 0) {
            if (m_held) ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
        }
    }

    ConfigWriteLock(const ConfigWriteLock &) = delete;
    ConfigWriteLock &operator=(const ConfigWriteLock &) = delete;

    bool acquired() const { return m_held; }

private:
    int  m_fd = -1;
    bool m_held = false;
};
