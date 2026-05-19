// Feature-conformance test for spec.md —
//
// Invariant 1 — debug.log lands 0600 after first setActive() under umask 0022.
// Invariant 2 — after clear() + setActive() again, new file is 0600.
// Invariant 3 — a pre-existing 0644 file is narrowed to 0600 on open.
// Invariant 4 — source uses the secureio helper, not a raw bitmask.
//
// Links against src/debuglog.cpp + src/secureio.h (header-only). No Qt GUI.
// Exit 0 = all invariants hold. Non-zero = regression.

#include "../../_support/expect.h"
#include "debuglog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>


#include <gtest/gtest.h>
ANTS_TEST_SCOPE();

namespace {



int statMode(const QString &path) {
    struct stat st{};
    if (::stat(path.toLocal8Bit().constData(), &st) != 0) return -1;
    return static_cast<int>(st.st_mode & 07777);
}

void runRuntimeChecks() {
    const mode_t oldUmask = ::umask(0022);

    // Capture prior XDG_DATA_HOME so we restore it at exit and don't
    // leave subsequent tests in the test_core bundle pointing at a
    // deleted directory.
    const bool hadXdg = qEnvironmentVariableIsSet("XDG_DATA_HOME");
    const QByteArray priorXdg = hadXdg ? qgetenv("XDG_DATA_HOME") : QByteArray();

    // Isolated XDG_DATA_HOME so parallel runs don't collide and so we
    // don't touch a real user log.
    const QString xdg =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-debuglog-perms-")
        + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(xdg);
    qputenv("XDG_DATA_HOME", xdg.toLocal8Bit());

    const QString expected = xdg + QStringLiteral("/ants-terminal/debug.log");

    // Start disabled then activate: the first Activate opens the file.
    DebugLog::setActive(0);

    // Invariant 1 — fresh open under umask 0022 lands 0600.
    DebugLog::setActive(DebugLog::Events);
    expect(QFileInfo::exists(expected),
           "I1/file-exists-after-setActive", expected);
    int mode = statMode(expected);
    expect(mode == 0600,
           "I1/fresh-open-perms-are-0600",
           QStringLiteral("expected 0600, got 0%1")
               .arg(mode, 4, 8, QLatin1Char('0')));

    // Invariant 2 — clear() keeps logging open at 0600 (ANTS-1190).
    // Pre-ANTS-1190, clear() only removed the file and left the
    // handle closed, silently dropping every subsequent log line.
    // The fix routes clear() through the same open-helper as
    // setActive(), so the file is recreated immediately.
    DebugLog::clear();
    expect(QFileInfo::exists(expected),
           "I2/file-still-open-after-clear");
    mode = statMode(expected);
    expect(mode == 0600,
           "I2/clear-reopen-perms-are-0600",
           QStringLiteral("expected 0600, got 0%1")
               .arg(mode, 4, 8, QLatin1Char('0')));
    // setActive again is idempotent — file is already open. Run it
    // anyway to assert that the perms hold across a setActive call
    // following a clear-while-active.
    DebugLog::setActive(DebugLog::Events);
    expect(QFileInfo::exists(expected),
           "I2/file-stays-after-reactivate");
    mode = statMode(expected);
    expect(mode == 0600,
           "I2/reactivate-perms-still-0600",
           QStringLiteral("expected 0600, got 0%1")
               .arg(mode, 4, 8, QLatin1Char('0')));

    // Invariant 3 — pre-existing 0644 file is narrowed on open.
    DebugLog::setActive(0);
    DebugLog::clear();
    QDir().mkpath(QFileInfo(expected).absolutePath());
    QFile pre(expected);
    expect(pre.open(QIODevice::WriteOnly | QIODevice::Truncate),
           "I3/plant-precondition-writable");
    pre.write("pre-existing line from an older build\n");
    pre.close();
    ::chmod(expected.toLocal8Bit().constData(), 0644);
    expect(statMode(expected) == 0644,
           "I3/precondition-file-is-0644");
    DebugLog::setActive(DebugLog::Events);
    mode = statMode(expected);
    expect(mode == 0600,
           "I3/stale-wider-perms-narrowed-on-open",
           QStringLiteral("expected 0600 (narrowed from 0644), got 0%1")
               .arg(mode, 4, 8, QLatin1Char('0')));

    // Cleanup.
    DebugLog::setActive(0);
    DebugLog::clear();
    QFile::remove(expected);
    QDir().rmpath(QFileInfo(expected).absolutePath());
    QDir().rmdir(xdg);
    ::umask(oldUmask);

    // Restore prior XDG_DATA_HOME so the next test in the bundle isn't
    // pointing at our now-removed temp dir.
    if (hadXdg) qputenv("XDG_DATA_HOME", priorXdg);
    else qunsetenv("XDG_DATA_HOME");
}

void runSourceChecks() {
    const QString path = QStringLiteral(SRC_DEBUGLOG_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] I4/source-open: cannot read %s\n",
                     qUtf8Printable(path));
        expect(false, "setup-error", "");
        return;
    }
    const QString src = QString::fromUtf8(f.readAll());
    f.close();

    expect(src.contains(QStringLiteral("#include \"secureio.h\"")),
           "I4/includes-secureio-header",
           QStringLiteral("src/debuglog.cpp must include secureio.h "
                          "to reach setOwnerOnlyPerms"));
    expect(src.contains(QStringLiteral("setOwnerOnlyPerms(s_file)")) ||
               src.contains(QStringLiteral("setOwnerOnlyPerms(path)")),
           "I4/uses-helper-not-raw-bitmask",
           QStringLiteral("debuglog.cpp must narrow perms via "
                          "setOwnerOnlyPerms(...), not a raw "
                          "setPermissions bitmask"));
    // Both fd-level + path-level calls expected — doubles as the
    // stale-wider-perms narrowing path covered by I3.
    expect(src.contains(QStringLiteral("setOwnerOnlyPerms(s_file)")),
           "I4/fd-level-call-present");
    expect(src.contains(QStringLiteral("setOwnerOnlyPerms(path)")),
           "I4/path-level-call-present");
}

}  // namespace

static int runMain() {
    expect_reset();
    // QCoreApplication is owned by bundle_main (ANTS-1217); this helper
    // takes no argc/argv since it doesn't need to forward to Qt.
    runRuntimeChecks();
    runSourceChecks();

    return expect_finish();
}

TEST(DebuglogPerms, Main) {
    ASSERT_EQ(0, runMain());
}
