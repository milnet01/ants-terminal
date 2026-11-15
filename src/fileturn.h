// FileTurn — one writer at a time on a shared file, in this process and
// across processes (each Claude Code session runs its own ants-mcpd).
//
// The turn is an exclusive flock(2) on a lock file named for `key`, in
// <GenericDataLocation>/ants-terminal/<folder>/, so no lock file lands in a
// project. flock excludes between descriptors, so it orders this process's
// threads and other processes alike, and the kernel drops it when its holder
// dies. The shape is RoadmapWriterTurn's (ANTS-5420). A caller that cannot
// have the turn within its wait refuses `file_busy`
// (docs/standards/mcp-error-codes.md § 2). Users: changelog_log (ANTS-6468),
// run_trace finish (ANTS-6469).
//
// No lock file (the folder cannot be made) means no ordering, as before the
// turn existed: held() is true, so a broken data folder never blocks writes.

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QThread>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

class FileTurn {
public:
    static constexpr int kDefaultWaitMs = 5000;

    FileTurn(const QString &folder, const QString &key,
             int waitMs = kDefaultWaitMs) {
        m_fd = openLock(folder, key);
        if (m_fd < 0) {
            m_held = true;
            return;
        }
        const int wait = waitOverrideForTest() > 0 ? waitOverrideForTest() : waitMs;
        const QDeadlineTimer deadline(std::max(0, wait));
        for (;;) {
            if (::flock(m_fd, LOCK_EX | LOCK_NB) == 0) {
                m_held = true;
                return;
            }
            if (errno != EINTR && errno != EWOULDBLOCK) break;
            if (deadline.hasExpired()) break;
            QThread::msleep(5);
        }
        ::close(m_fd);
        m_fd = -1;
    }
    ~FileTurn() {
        if (m_fd >= 0) ::close(m_fd);   // closing the descriptor drops the flock
    }
    FileTurn(const FileTurn &) = delete;
    FileTurn &operator=(const FileTurn &) = delete;

    bool held() const { return m_held; }

    // The lock file's descriptor, opened (and created) but not locked; -1 when
    // the folder cannot be made. Public so a test can hold a turn itself.
    static int openLock(const QString &folder, const QString &key) {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
            QStringLiteral("/ants-terminal/") + folder;
        if (!QDir().mkpath(dir)) return -1;
        ::chmod(QFile::encodeName(dir).constData(), 0700);
        const QString path = dir + QLatin1Char('/') +
            QString::fromLatin1(QCryptographicHash::hash(
                key.toUtf8(), QCryptographicHash::Sha1).toHex()) +
            QStringLiteral(".lock");
        return ::open(QFile::encodeName(path).constData(),
                      O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    }

    // Test only: a positive value replaces every caller's wait; 0 restores them.
    static void setWaitMsForTest(int ms) { waitOverrideForTest() = ms; }

private:
    static int &waitOverrideForTest() {
        static int ms = 0;
        return ms;
    }
    int  m_fd = -1;
    bool m_held = false;
};
