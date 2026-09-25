// ANTS-5340 — the About dialog's ants-mcpd line. Contract: spec.md here.

#include "build_info.h"
#include "mcpdversion.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifndef ANTS_MCPD_BIN
#error "ANTS_MCPD_BIN compile definition required"
#endif

namespace {

// An executable file at `path`. Its content never runs.
void makeExecutable(const QString &path) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("#!/bin/sh\n");
    f.close();
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                     | QFileDevice::ExeOwner);
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

QByteArray configNaming(const QString &command) {
    return QByteArray(R"({"mcpServers":{"ants":{"type":"stdio","command":")")
        + command.toUtf8() + R"(","args":[]}}})";
}

}  // namespace

// INV-1
TEST(McpdAboutVersion, Inv1VersionFlagPrintsOneLine) {
    QProcess p;
    p.start(QStringLiteral(ANTS_MCPD_BIN), {QStringLiteral("--version")});
    ASSERT_TRUE(p.waitForStarted(5000));
    // stdin stays open: a --version that read it would hang here.
    ASSERT_TRUE(p.waitForFinished(5000)) << "--version waited on stdin";
    EXPECT_EQ(p.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(p.exitCode(), 0);
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    EXPECT_EQ(out, mcpd::versionLine() + QLatin1Char('\n'));
    EXPECT_TRUE(mcpd::versionLine().startsWith(
        QStringLiteral("ants-mcpd " ANTS_VERSION " ")))
        << mcpd::versionLine().toStdString();
    EXPECT_TRUE(mcpd::versionLine().contains(
        QString::fromLatin1(ANTS_BUILD_COMMIT)));
}

// INV-2
TEST(McpdAboutVersion, Inv2ConfigCommandWins) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString registered = tmp.filePath(QStringLiteral("registered-mcpd"));
    const QString appDir = tmp.filePath(QStringLiteral("app"));
    ASSERT_TRUE(QDir().mkpath(appDir));
    makeExecutable(registered);
    makeExecutable(appDir + QStringLiteral("/ants-mcpd"));
    const QString cfg = tmp.filePath(QStringLiteral("claude.json"));
    writeFile(cfg, configNaming(registered));
    EXPECT_EQ(mcpd::locateBinary(cfg, appDir), registered);
}

// INV-3
TEST(McpdAboutVersion, Inv3FallsBackToSibling) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString appDir = tmp.filePath(QStringLiteral("app"));
    ASSERT_TRUE(QDir().mkpath(appDir));
    const QString sibling = appDir + QStringLiteral("/ants-mcpd");
    makeExecutable(sibling);
    const QString notExec = tmp.filePath(QStringLiteral("not-executable"));
    writeFile(notExec, "x");

    const QString cfg = tmp.filePath(QStringLiteral("claude.json"));
    EXPECT_EQ(mcpd::locateBinary(cfg, appDir), sibling) << "missing file";
    writeFile(cfg, "not json");
    EXPECT_EQ(mcpd::locateBinary(cfg, appDir), sibling) << "not JSON";
    writeFile(cfg, R"({"mcpServers":{"other":{"command":"/bin/true"}}})");
    EXPECT_EQ(mcpd::locateBinary(cfg, appDir), sibling) << "no ants entry";
    writeFile(cfg, configNaming(notExec));
    EXPECT_EQ(mcpd::locateBinary(cfg, appDir), sibling) << "not executable";
}

// INV-4
TEST(McpdAboutVersion, Inv4FallsBackToPath) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString expected =
        QStandardPaths::findExecutable(QStringLiteral("ants-mcpd"));
    EXPECT_EQ(mcpd::locateBinary(tmp.filePath(QStringLiteral("none.json")),
                                 tmp.path()),
              expected);
}

namespace {

// A running ants-mcpd child. It serves stdio, so it runs until stdin closes.
struct RunningChild {
    QProcess p;
    explicit RunningChild(const QString &binary) {
        p.start(binary, {});
        p.waitForStarted(5000);
    }
    ~RunningChild() {
        p.closeWriteChannel();  // EOF: ants-mcpd quits on its own
        if (!p.waitForFinished(5000)) p.kill();
    }
};

const mcpd::RunningCopy *findPid(const QList<mcpd::RunningCopy> &copies, qint64 pid) {
    for (const auto &c : copies)
        if (c.pid == pid) return &c;
    return nullptr;
}

}  // namespace

// INV-6
TEST(McpdAboutVersion, Inv6ListsARunningCopy) {
    RunningChild child{QStringLiteral(ANTS_MCPD_BIN)};
    ASSERT_EQ(child.p.state(), QProcess::Running);
    const auto copies = mcpd::runningCopies(5000);
    const mcpd::RunningCopy *c = findPid(copies, child.p.processId());
    ASSERT_NE(c, nullptr) << "the child ants-mcpd was not listed";
    EXPECT_EQ(c->version, mcpd::versionLine());
    EXPECT_FALSE(c->replaced);
    EXPECT_TRUE(c->ancestors.contains(QCoreApplication::applicationPid()));
    EXPECT_FALSE(mcpd::isStale(*c, mcpd::versionLine()));
}

// INV-7
TEST(McpdAboutVersion, Inv7ReplacedCopyKeepsItsVersion) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString copy = tmp.filePath(QStringLiteral("ants-mcpd"));
    ASSERT_TRUE(QFile::copy(QStringLiteral(ANTS_MCPD_BIN), copy));
    RunningChild child{copy};
    ASSERT_EQ(child.p.state(), QProcess::Running);
    // Replace the file the way a rebuild does: a new file renamed over it.
    const QString next = tmp.filePath(QStringLiteral("next"));
    makeExecutable(next);
    ASSERT_TRUE(QFile::remove(copy));
    ASSERT_TRUE(QFile::rename(next, copy));

    const auto copies = mcpd::runningCopies(5000);
    const mcpd::RunningCopy *c = findPid(copies, child.p.processId());
    ASSERT_NE(c, nullptr) << "the replaced copy was not listed";
    EXPECT_TRUE(c->replaced);
    EXPECT_EQ(c->version, mcpd::versionLine());
    EXPECT_TRUE(mcpd::isStale(*c, mcpd::versionLine()));
}

// INV-8
TEST(McpdAboutVersion, Inv8IsStale) {
    const QString disk = QStringLiteral("ants-mcpd 1.0.0 · a");
    mcpd::RunningCopy c;
    c.version = disk;
    EXPECT_FALSE(mcpd::isStale(c, disk)) << "matching copy";
    c.replaced = true;
    EXPECT_TRUE(mcpd::isStale(c, disk)) << "replaced";
    c.replaced = false;
    c.version.clear();
    EXPECT_TRUE(mcpd::isStale(c, disk)) << "no version line";
    c.version = QStringLiteral("ants-mcpd 1.0.0 · b");
    EXPECT_TRUE(mcpd::isStale(c, disk)) << "different version";
}

// INV-5
TEST(McpdAboutVersion, Inv5QueryVersion) {
    ASSERT_FALSE(mcpd::versionLine().isEmpty());
    EXPECT_EQ(mcpd::queryVersion(QStringLiteral(ANTS_MCPD_BIN), 5000),
              mcpd::versionLine());
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_TRUE(mcpd::queryVersion(tmp.filePath(QStringLiteral("absent")), 2000)
                    .isEmpty());
}
