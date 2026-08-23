// ANTS-4625 — the Qt-version guard holds.
//
// A Qt point upgrade can silently poison an incremental build tree: RPM
// preserves upstream header mtimes, so headers newer in CONTENT can be older
// in MTIME than the objects compiled from their predecessors. Ninja is
// mtime-based, so it rebuilds nothing and links new libraries against stale
// objects. See spec.md for the measurement.
//
// Everything here drives the real cmake script through `cmake -P`, against
// committed fixture headers. Nothing re-implements the comparison — a replica
// would agree with itself.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QProcess>
#include <QString>
#include <QStringList>

#include "../../_support/expect.h"

ANTS_TEST_SCOPE();

#if !defined(ANTS_QT_GUARD_SCRIPT) || !defined(ANTS_QT_GUARD_FIXTURES)
#error "qt_version_guard needs ANTS_QT_GUARD_SCRIPT and ANTS_QT_GUARD_FIXTURES"
#endif

namespace {

struct RunResult {
    int     exitCode = -1;
    QString output;
};

// Invoke the guard exactly as the build does: `cmake -P <script>` with the
// two -D vars. Returns the merged stdout+stderr so INV-4 can read the message.
RunResult runGuard(const QString &expectedVersion, const QString &qconfigPath) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("cmake"),
            {QStringLiteral("-DEXPECTED_QT_VERSION=") + expectedVersion,
             QStringLiteral("-DQCONFIG_FILE=") + qconfigPath,
             QStringLiteral("-P"), QStringLiteral(ANTS_QT_GUARD_SCRIPT)});
    if (!p.waitForFinished(30000)) {
        p.kill();
        return {-1, QStringLiteral("(guard did not finish within 30s)")};
    }
    return {p.exitCode(), QString::fromUtf8(p.readAll())};
}

QString fixture(const char *name) {
    return QStringLiteral(ANTS_QT_GUARD_FIXTURES) + QLatin1Char('/')
           + QString::fromLatin1(name);
}

}  // namespace

// INV-1 / INV-2 — the script exists, is invocable, and a matching pair passes.
//
// INV-1 is not redundant with INV-3. `cmake -P` on a MISSING file also exits
// non-zero, so without a case that requires exit 0 from the real script, an
// absent guard would satisfy every refusal case and the suite would be green
// on nothing at all.
TEST(QtVersionGuard, Inv1And2MatchingVersionPasses) {
    expect_reset();
    ASSERT_TRUE(QFile::exists(QStringLiteral(ANTS_QT_GUARD_SCRIPT)))
        << "INV-1: guard script missing at " << ANTS_QT_GUARD_SCRIPT;

    const RunResult r = runGuard(QStringLiteral("6.11.2"),
                                 fixture("qconfig_6_11_2.h"));
    expect(r.exitCode == 0,
           (std::string("INV-2: matching version must pass (exit ")
            + std::to_string(r.exitCode) + ", output: "
            + r.output.toStdString() + ")").c_str());
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — a differing version refuses. The pair is the real one: a PATCH bump,
// which a major.minor-only comparison would wave through.
TEST(QtVersionGuard, Inv3PatchDifferenceRefuses) {
    expect_reset();
    const RunResult r = runGuard(QStringLiteral("6.11.1"),
                                 fixture("qconfig_6_11_2.h"));
    expect(r.exitCode != 0,
           "INV-3: 6.11.1-stamped tree against a 6.11.2 header must REFUSE");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — the refusal is actionable. The incident's cost was not knowing what
// to do, so a message that omits the remedy fails the point of the guard.
TEST(QtVersionGuard, Inv4RefusalNamesVersionsAndRemedy) {
    expect_reset();
    const RunResult r = runGuard(QStringLiteral("6.11.1"),
                                 fixture("qconfig_6_11_2.h"));
    ASSERT_NE(0, r.exitCode) << "INV-4 setup: expected a refusal to read";

    expect(r.output.contains(QStringLiteral("6.11.1")),
           "INV-4: refusal names the version the tree was built against");
    expect(r.output.contains(QStringLiteral("6.11.2")),
           "INV-4: refusal names the version now installed");
    expect(r.output.contains(QStringLiteral("-t clean")),
           "INV-4: refusal names the remedy (`ninja -t clean`)");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — cannot-read must not read as nothing-wrong.
TEST(QtVersionGuard, Inv5UnreadableOrVersionlessRefuses) {
    expect_reset();

    const RunResult noVersion = runGuard(QStringLiteral("6.11.2"),
                                         fixture("qconfig_no_version.h"));
    expect(noVersion.exitCode != 0,
           "INV-5: a header with no QT_VERSION_STR must refuse, not pass");

    const RunResult missing = runGuard(QStringLiteral("6.11.2"),
                                       fixture("this_file_does_not_exist.h"));
    expect(missing.exitCode != 0,
           "INV-5: an absent header must refuse, not pass");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — the guard is actually invoked by the build.
//
// The script's own cases cannot see this: a perfectly correct guard that
// nothing runs is exactly the state ANTS-4625 exists to prevent.
TEST(QtVersionGuard, Inv6WiredIntoTheBuildAsAnAllTarget) {
    expect_reset();
    QFile f(QStringLiteral(ANTS_QT_GUARD_CMAKELISTS));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "INV-6: cannot read CMakeLists.txt";
    const QByteArray cm = f.readAll();

    expect(cm.contains("ants_qt_version_guard"),
           "INV-6: guard target declared in CMakeLists.txt");
    expect(cm.contains("QtVersionGuard.cmake"),
           "INV-6: CMakeLists invokes the guard script by name");

    // The target must be ALL, or it never runs on an ordinary build.
    const int t = cm.indexOf("add_custom_target(ants_qt_version_guard");
    ASSERT_GE(t, 0) << "INV-6: guard is not an add_custom_target";
    const int close = cm.indexOf("VERBATIM", t);
    ASSERT_GE(close, 0) << "INV-6: guard target block not terminated as expected";
    expect(cm.mid(t, close - t).contains(" ALL"),
           "INV-6: guard target is declared ALL so it runs every build");
    EXPECT_EQ(0, expect_failures());
}
