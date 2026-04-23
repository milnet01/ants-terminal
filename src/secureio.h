#pragma once

#include <QDateTime>
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

// Rotate a corrupt JSON-ish file aside so the next save() doesn't
// clobber it. Used by Config, Claude allowlist, and SessionManager —
// the 0.7.12 /indie-review sweep flagged the same silent-data-loss
// pattern (parse failure → next save overwrites user bytes) in all
// three, and the three reviewers asked for a shared helper rather
// than three copies of the same ~30-line block.
//
// Behavior:
//   - Builds `<path>.corrupt-<ms_timestamp>` and tries QFile::copy.
//   - On collision (QFile::copy refuses to overwrite) retries with
//     `-1`, `-2`, ... up to 10 suffixes. Races beyond that are
//     pathological (same-millisecond multi-launch) and fall through
//     to the failure path.
//   - Returns the chosen backup path on success, empty QString on
//     failure (disk full, perms, 10 ms collisions).
//
// Callers log success/failure using their own DebugLog category.
inline QString rotateCorruptFileAside(const QString &path) {
    const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    for (int attempt = 0; attempt < 10; ++attempt) {
        QString candidate = path + QStringLiteral(".corrupt-")
                          + QString::number(stamp);
        if (attempt > 0) {
            candidate += QStringLiteral("-") + QString::number(attempt);
        }
        if (QFile::copy(path, candidate)) {
            return candidate;
        }
    }
    return QString();
}
