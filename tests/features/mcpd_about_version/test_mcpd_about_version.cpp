// ANTS-5340 — the About dialog's ants-mcpd line. Contract: spec.md here.

#include "build_info.h"
#include "mcpdversion.h"

#include <gtest/gtest.h>

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
