// Fixture: setPermissions_pair_no_helper — raw 0600 bitmask, bypassing
// the setOwnerOnlyPerms() helper in src/secureio.h. Flagged so the next
// contributor adds one more surface that would silently widen if the
// bitmask is mistyped (ReadOwner | ReadOther instead of ReadOwner |
// WriteOwner).

#include <QFile>
#include <QFileDevice>
#include <QString>

static void writeSecretA(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write("api-key=xxx");
    f.close();
    // Raw bitmask — should use setOwnerOnlyPerms(f).
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); // @expect setPermissions_pair_no_helper
}

static void writeSecretB(const QString &path) {
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("token=yyy");
    f.close();
    // Static-method shape — same hygiene violation.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner); // @expect setPermissions_pair_no_helper
}
