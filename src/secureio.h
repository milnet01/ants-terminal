#pragma once

#include <QFile>
#include <QFileDevice>
#include <QString>

#include <sys/stat.h>
#include <unistd.h>

// Apply owner-only (0600) permissions to a file or local socket. The literal
// bitmask QFileDevice::ReadOwner | QFileDevice::WriteOwner was previously
// repeated at ~12 call sites across config, session, audit, claude-hook and
// remote-control persistence — one typo (ReadOther, ReadGroup) away from
// silently widening access to a file that may hold an API key. Routing every
// permission set through these named helpers makes the 0600 intent the only
// way to call it.
//
// 0.7.31: split into two headers. Perms helpers stay here; corrupt-file
// rotation + cooperative write-lock moved to configbackup.h. Both files
// are header-only and inline; downstream call sites pick the include
// they need rather than dragging the whole sweep along.
//
// ants-audit: disable-file=setPermissions_pair_no_helper
// ^ this header is the helper: the raw 0600 bitmask is the definition, not
//   a call site. The rule nudges all *other* call sites toward these helpers.

inline bool setOwnerOnlyPerms(QFileDevice &f) {
    return f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

inline bool setOwnerOnlyPerms(const QString &path) {
    return QFile::setPermissions(path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

// ANTS-1132 — defence-in-depth before unlinking a Unix-domain socket
// path. Refuse to remove anything that isn't a lstat()'d socket owned
// by the running user. XDG_RUNTIME_DIR is 0700 and already safe; /tmp
// fallback paths and per-pid /tmp paths (Claude hook + MCP) live in
// shared /tmp where a prior-session symlink — or a same-uid
// confusion between two apps sharing a UID-suffixed name — could
// otherwise cause us to unlink an unrelated file. lstat (not stat)
// so a symlink is reported as a symlink and gets refused.
//
// Promoted from the file-scope static in remotecontrol.cpp by
// ANTS-1132 (0.7.66) so the Claude hook + MCP server start paths
// can also gate their `removeServer()` calls behind it. Same trust
// model as remotecontrol; the comment block at remotecontrol.cpp's
// trust-model section applies here too.
inline bool safeToUnlinkLocalSocket(const QString &path) {
    const QByteArray bytes = path.toLocal8Bit();
    struct stat st;
    if (::lstat(bytes.constData(), &st) != 0) {
        // ENOENT is the common case — nothing to remove, nothing to guard.
        return true;
    }
    if (!S_ISSOCK(st.st_mode)) return false;
    if (st.st_uid != ::getuid()) return false;
    return true;
}
