// Feature-conformance test for spec.md (ANTS-5527).
//
// INV-1..3 drive the pure HostExec::wrap. INV-4 scrapes the MCP subprocess
// sites for a bare tool name handed straight to QProcess.

#include "hostexec.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QRegularExpression>

namespace {

QProcessEnvironment envOf(std::initializer_list<std::pair<const char *, const char *>> kv) {
    QProcessEnvironment e;
    for (const auto &p : kv) e.insert(QString::fromUtf8(p.first), QString::fromUtf8(p.second));
    return e;
}

}  // namespace

// INV-1
TEST(FlatpakHostTools, OutsideSandboxUnchanged) {
    const auto l = HostExec::wrap(QStringLiteral("git"),
                                  {QStringLiteral("status")},
                                  QStringLiteral("/repo"),
                                  envOf({{"GIT_OPTIONAL_LOCKS", "0"}}),
                                  false, envOf({}));
    EXPECT_EQ(l.program, QStringLiteral("git"));
    EXPECT_EQ(l.args, QStringList{QStringLiteral("status")});
}

// INV-2
TEST(FlatpakHostTools, InsideSandboxGoesThroughFlatpakSpawn) {
    const auto l = HostExec::wrap(QStringLiteral("rg"),
                                  {QStringLiteral("--json"), QStringLiteral("foo")},
                                  QStringLiteral("/home/u/proj"),
                                  QProcessEnvironment(), true, envOf({}));
    EXPECT_EQ(l.program, QStringLiteral("flatpak-spawn"));
    const QStringList want{QStringLiteral("--host"), QStringLiteral("--watch-bus"),
                           QStringLiteral("--directory=/home/u/proj"),
                           QStringLiteral("--"), QStringLiteral("rg"),
                           QStringLiteral("--json"), QStringLiteral("foo")};
    EXPECT_EQ(l.args, want);
}

TEST(FlatpakHostTools, NoDirectoryWhenDirEmpty) {
    const auto l = HostExec::wrap(QStringLiteral("git"), {},
                                  QString(), QProcessEnvironment(), true, envOf({}));
    EXPECT_FALSE(l.args.join(' ').contains(QStringLiteral("--directory")));
    EXPECT_EQ(l.args.last(), QStringLiteral("git"));
}

// INV-3
TEST(FlatpakHostTools, CallerSetVariablesCrossAsEnv) {
    const auto base = envOf({{"PATH", "/app/bin:/usr/bin"}, {"LANG", "C"}});
    const auto env  = envOf({{"PATH", "/app/bin:/usr/bin"}, {"LANG", "en_ZA.UTF-8"},
                             {"GIT_OPTIONAL_LOCKS", "0"}});
    const auto l = HostExec::wrap(QStringLiteral("git"), {QStringLiteral("status")},
                                  QString(), env, true, base);
    const int sep = l.args.indexOf(QStringLiteral("--"));
    ASSERT_GE(sep, 0);
    const QStringList opts = l.args.mid(0, sep);
    EXPECT_TRUE(opts.contains(QStringLiteral("--env=GIT_OPTIONAL_LOCKS=0")));
    EXPECT_TRUE(opts.contains(QStringLiteral("--env=LANG=en_ZA.UTF-8")));
    EXPECT_FALSE(opts.join(' ').contains(QStringLiteral("PATH")));
}

TEST(FlatpakHostTools, EmptyEnvAddsNoEnvToken) {
    const auto l = HostExec::wrap(QStringLiteral("ctest"), {}, QString(),
                                  QProcessEnvironment(), true,
                                  envOf({{"PATH", "/usr/bin"}}));
    EXPECT_FALSE(l.args.join(' ').contains(QStringLiteral("--env=")));
}

// INV-4
TEST(FlatpakHostTools, McpSitesUseTheHelper) {
    const char *files[] = {
        "src/gitwrap.cpp", "src/remotecontrol_workspace.cpp",
        "src/remotecontrol_state.cpp", "src/remotecontrol_review.cpp",
        "src/remotecontrol_roadmap_backfill.cpp", "src/debtsweepengine.cpp",
        "src/auditscope.cpp", "src/auditcache.cpp", "src/auditengine.cpp",
        "src/projectsettings.cpp",
    };
    static const QRegularExpression bare(
        QString::fromUtf8(R"re(\.start\(\s*(QStringLiteral\()?"(rg|git|ctest|bash)")re"));
    for (const char *rel : files) {
        QFile f(QStringLiteral(ANTS_SOURCE_DIR "/") + QString::fromUtf8(rel));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << rel;
        const QString text = QString::fromUtf8(f.readAll());
        EXPECT_FALSE(bare.match(text).hasMatch()) << rel << " starts a bare tool";
        EXPECT_TRUE(text.contains(QStringLiteral("HostExec::start("))) << rel;
    }
}

// INV-5 (ANTS-5598)
TEST(FlatpakHostTools, TerminalWindowsUseTheHelper) {
    const char *files[] = {
        "src/diffviewer.cpp", "src/roadmapdialog.cpp",
        "src/auditdialog.cpp", "src/mainwindow.cpp",
    };
    static const QRegularExpression bare(
        QString::fromUtf8(R"re((\.|->)start\(\s*(QStringLiteral\()?"(git|gh)")re"));
    static const QRegularExpression setProg(
        QString::fromUtf8(R"re(setProgram\(\s*(QStringLiteral\()?"(git|gh)")re"));
    for (const char *rel : files) {
        QString text;
        if (QString::fromUtf8(rel) == QLatin1String("src/mainwindow.cpp")) {
            // ANTS-1677 INV-3 — MainWindow spans its source list.
            text = QString::fromStdString(
                ants_test::slurpSourceList(ANTS_MAINWINDOW_SOURCES));
        } else {
            QFile f(QStringLiteral(ANTS_SOURCE_DIR "/") + QString::fromUtf8(rel));
            ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << rel;
            text = QString::fromUtf8(f.readAll());
        }
        ASSERT_FALSE(text.isEmpty()) << rel;
        EXPECT_FALSE(bare.match(text).hasMatch()) << rel << " starts a bare git/gh";
        int programs = 0;
        for (auto it = setProg.globalMatch(text); it.hasNext(); it.next()) ++programs;
        EXPECT_GE(int(text.count(QStringLiteral("HostExec::start(*"))), programs)
            << rel << ": a setProgram(\"git\"|\"gh\") process starts without HostExec";
    }
}
