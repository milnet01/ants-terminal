// Fixture: setPermissions_pair_no_helper — correct 0600 handling.
//
// Two safe shapes:
//   1. Route through setOwnerOnlyPerms() — hides the bitmask behind a
//      named helper so a typo that adds ReadGroup / ReadOther can't
//      silently widen access.
//   2. Legitimate non-0600 permissions (e.g. a hook script that must be
//      world-executable) — pattern intentionally does NOT match when the
//      bitmask continues with more `|` flags beyond WriteOwner.

#include <QFile>
#include <QFileDevice>
#include <QString>

inline bool setOwnerOnlyPerms(QFileDevice &f);
inline bool setOwnerOnlyPerms(const QString &path);

static void writeSecretViaHelper(const QString &path) {
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("api-key=xxx");
    f.close();
    setOwnerOnlyPerms(f);
}

static void writeHookScript(const QString &path) {
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("#!/bin/sh\n");
    f.close();
    // 0755: world-readable + owner-executable hook. Extra `|` flags mean
    // the strict pattern (paren immediately after WriteOwner) does not fire.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                   | QFileDevice::ExeOwner  | QFileDevice::ReadGroup
                   | QFileDevice::ReadOther | QFileDevice::ExeGroup
                   | QFileDevice::ExeOther);
}
