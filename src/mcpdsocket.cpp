#include "mcpdsocket.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>

namespace mcpd {

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
    const QDir tmp(QDir::tempPath());
    const QStringList entries = tmp.entryList(
        QStringList{QStringLiteral("ants-terminal-mcp-*")},
        QDir::System | QDir::Files | QDir::Hidden);
    for (const QString &name : entries) {
        const QString path = tmp.filePath(name);
        struct stat st{};
        if (::lstat(QFile::encodeName(path).constData(), &st) != 0) continue;
        if (!S_ISSOCK(st.st_mode)) continue;
        if (st.st_uid != expectedUid) { ++foreign; continue; }
        bool ok = false;
        const long pid = name.section(QLatin1Char('-'), -1).toLong(&ok);
        const int live = (ok && pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0) ? 1 : 0;
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
            : QStringLiteral("no /tmp/ants-terminal-mcp-* socket found");
    }
    return best;
}

}  // namespace mcpd
