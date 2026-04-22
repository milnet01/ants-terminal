#pragma once

#include <QFile>
#include <QFileDevice>
#include <QString>

// Apply owner-only (0600) permissions to a file or local socket. The literal
// bitmask QFileDevice::ReadOwner | QFileDevice::WriteOwner was previously
// repeated at ~12 call sites across config, session, audit, claude-hook and
// remote-control persistence — one typo (ReadOther, ReadGroup) away from
// silently widening access to a file that may hold an API key. Routing every
// permission set through these named helpers makes the 0600 intent the only
// way to call it.
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
