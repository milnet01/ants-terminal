#include "mcpdsocket.h"

#include "configpaths.h"
#include "secureio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>

namespace mcpd {

namespace {

// ANTS-5236 § 2.4 — every terminal MCP socket name (mcp-<pid>) in the
// private runtime directory. The legacy /tmp name is no longer read (ANTS-5587).
QStringList terminalSocketCandidates() {
    QStringList paths;
    const QString dirPath = ConfigPaths::antsRuntimeDir();
    if (dirPath.isEmpty()) return paths;
    const QDir dir(dirPath);
    for (const QString &name : dir.entryList(QStringList{QStringLiteral("mcp-*")},
             QDir::System | QDir::Files | QDir::Hidden))
        paths << dir.filePath(name);
    return paths;
}

// The pid a socket name ends in, or -1.
long socketPid(const QString &path) {
    bool ok = false;
    const long pid = QFileInfo(path).fileName().section(QLatin1Char('-'), -1).toLong(&ok);
    return ok ? pid : -1;
}

}  // namespace

// lstat, never stat: /tmp is world-writable, so another uid can plant a
// symlink at an ants-terminal-mcp-<pid> name pointing at a socket this
// user owns, and stat would follow it and pass both checks.
bool socketOwnedBy(const QString &path, uid_t expectedUid) {
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) return false;
    return S_ISSOCK(st.st_mode) && st.st_uid == expectedUid;
}

bool peerUidIs(int fd, uid_t expectedUid) {
    if (fd < 0) return false;
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
        len != sizeof(cred))
        return false;
    return cred.uid == expectedUid;
}

QString pickTerminalSocket(uid_t expectedUid, QString *whyNot) {
    const QByteArray override = qgetenv("ANTS_MCP_SOCKET");
    if (!override.isEmpty()) {
        QString path = QString::fromLocal8Bit(override);
        if (!QFileInfo::exists(path)) {
            if (whyNot) *whyNot = QStringLiteral("ANTS_MCP_SOCKET names %1, which does not exist").arg(path);
            return {};
        }
        if (!socketOwnedBy(path, expectedUid)) {
            if (whyNot) *whyNot = QStringLiteral("ANTS_MCP_SOCKET names %1, which is not a socket owned by uid %2 (uid check)").arg(path).arg(expectedUid);
            return {};
        }
        return path;
    }
    QString best;
    int bestLive = -1;
    qint64 bestMtime = 0;
    int foreign = 0;
    for (const QString &path : terminalSocketCandidates()) {
        struct stat st{};
        if (::lstat(QFile::encodeName(path).constData(), &st) != 0) continue;
        if (!S_ISSOCK(st.st_mode)) continue;
        if (st.st_uid != expectedUid) { ++foreign; continue; }
        const long pid = socketPid(path);
        const int live = (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0) ? 1 : 0;
        const qint64 mtime = static_cast<qint64>(st.st_mtime);
        if (live > bestLive || (live == bestLive && mtime > bestMtime)) {
            best = path;
            bestLive = live;
            bestMtime = mtime;
        }
    }
    if (best.isEmpty() && whyNot) {
        *whyNot = foreign > 0
            ? QStringLiteral("no terminal socket owned by uid %1; %2 owned by another uid skipped (uid check)").arg(expectedUid).arg(foreign)
            : QStringLiteral("no mcp-* socket in %1").arg(ConfigPaths::antsRuntimeDir());
    }
    return best;
}

int reapStaleTerminalSockets(pid_t self) {
    int removed = 0;
    for (const QString &path : terminalSocketCandidates()) {
        const long pid = socketPid(path);
        if (pid <= 0 || pid == self) continue;
        // kill(pid, 0) succeeds for a live pid; EPERM is someone else's
        // live process. Only ESRCH says the owner is gone.
        if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) continue;
        if (QFileInfo::exists(path) && safeToUnlinkLocalSocket(path) &&
            QFile(path).remove())
            ++removed;
    }
    return removed;
}

}  // namespace mcpd
