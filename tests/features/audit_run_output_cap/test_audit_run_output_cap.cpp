// ANTS-5085 — the headless audit runner bounds tool output. Contract:
// spec.md beside this file.

#include "auditrunner.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace {

class ScopedPathPrepend {
public:
    explicit ScopedPathPrepend(const QString &dir) : m_old(qgetenv("PATH")) {
        QByteArray next = dir.toUtf8();
        if (!m_old.isEmpty()) { next += ':'; next += m_old; }
        qputenv("PATH", next);
    }
    ~ScopedPathPrepend() { qputenv("PATH", m_old); }

private:
    QByteArray m_old;
};

bool writeExecutable(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(body.toUtf8());
    f.close();
    return f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ExeOwner);
}

// A fake shellcheck printing `bytes` of 'a' to stdout.
AuditRunner::RunResult runFakeShellcheck(const QTemporaryDir &toolDir,
                                         const QTemporaryDir &projectDir,
                                         qint64 bytes) {
    const QString script = QStringLiteral(
        "#!/bin/sh\nhead -c %1 /dev/zero | tr '\\000' a\n").arg(bytes);
    if (!writeExecutable(toolDir.path() + QStringLiteral("/shellcheck"), script))
        return {};
    ScopedPathPrepend pathGuard(toolDir.path());
    AuditRunner::RunRequest req;
    req.projectRoot = projectDir.path();
    req.tools = {QStringLiteral("shellcheck")};
    req.capPerToolSeconds = 60;
    return AuditRunner::runAudit(req);
}

}  // namespace

// INV-1
TEST(AuditRunOutputCap, RunawayOutputIsCapped) {
    QTemporaryDir toolDir, projectDir;
    ASSERT_TRUE(toolDir.isValid() && projectDir.isValid());

    const auto r = runFakeShellcheck(toolDir, projectDir, 70LL * 1024 * 1024);
    ASSERT_TRUE(r.byTool.contains(QStringLiteral("shellcheck")))
        << "test setup: the fake shellcheck never ran — " << r.error.toStdString();
    EXPECT_EQ(r.byTool.value(QStringLiteral("shellcheck")).status,
              QStringLiteral("output_too_large"));
}

// INV-2
TEST(AuditRunOutputCap, SarifExcerptIsBounded) {
    QTemporaryDir toolDir, projectDir;
    ASSERT_TRUE(toolDir.isValid() && projectDir.isValid());

    const auto r = runFakeShellcheck(toolDir, projectDir, 1024LL * 1024);
    ASSERT_FALSE(r.sarifPath.isEmpty())
        << "test setup: no SARIF written — " << r.error.toStdString();
    QFile f(r.sarifPath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    int longest = -1;
    // writeSarif puts the raw-output excerpt on the run.
    for (const auto &run : root.value(QStringLiteral("runs")).toArray()) {
        const QJsonArray notifs = run.toObject()
            .value(QStringLiteral("toolExecutionNotifications")).toArray();
        for (const auto &n : notifs)
            longest = std::max<int>(longest, n.toObject().value(QStringLiteral("message"))
                .toObject().value(QStringLiteral("text")).toString().size());
    }
    ASSERT_GE(longest, 0) << "test setup: no toolExecutionNotifications in the SARIF";
    EXPECT_LE(longest, 64 * 1024) << "the SARIF embeds " << longest << " chars of raw output";
}
