// ANTS-5038 — behavioural regression test: AuditRunner::runAudit's per-tool
// cap must stop a tool's whole process tree, not just its own direct child.
// See spec.md INV-2.
//
// A fake "mypy" forks a long-lived sleeper, records the sleeper's pid to a
// file next to itself, and then just waits — outliving a 5 s per-tool cap
// on purpose. AuditRunner is expected to report it timed_out (it already
// does) AND to have actually stopped the sleeper (it does not, today):
// runAudit's per-tool cap calls proc->terminate() / proc->kill() on the
// direct QProcess only, so a tool that forked a helper leaves it running.
//
// "mypy" is picked deliberately: kKnownTools() lists it, but
// kAutoDetectTools() drops it (ANTS-3418), and no other test in this bundle
// requests it explicitly — so AuditRunner's process-wide 60s tool-resolve
// cache (keyed by tool name, shared across every TEST() in this binary) has
// nothing stale cached for it that our PATH override would otherwise lose to.

#include "auditrunner.h"
#include "../../_support/procwait.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QFileDevice>
#include <QIODevice>
#include <QString>
#include <QTemporaryDir>

#include <csignal>

namespace {

// Prepend `dir` to PATH for the scope's lifetime, restoring the prior value
// on destruction. AuditRunner resolves a tool name via
// QStandardPaths::findExecutable, which re-reads PATH on every call, so a
// fake "mypy" placed first on PATH is what gets resolved and spawned — the
// runner is driven end to end with no src/ change.
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
                            QFileDevice::ExeOwner  | QFileDevice::ReadGroup |
                            QFileDevice::ExeGroup  | QFileDevice::ReadOther |
                            QFileDevice::ExeOther);
}

}  // namespace

TEST(AuditRunnerGroupKill, PerToolCapStopsTheWholeToolTree) {
    QTemporaryDir toolDir;
    QTemporaryDir projectDir;
    ASSERT_TRUE(toolDir.isValid());
    ASSERT_TRUE(projectDir.isValid());

    const QString pidFile = toolDir.path() + QStringLiteral("/mypy.pid");
    const QString script = QStringLiteral(
        "#!/bin/sh\n"
        "sleep 300 &\n"
        "echo $! > \"%1\"\n"
        "wait\n").arg(pidFile);
    ASSERT_TRUE(writeExecutable(toolDir.path() + QStringLiteral("/mypy"), script))
        << "test setup: could not write the fake mypy script";

    ScopedPathPrepend pathGuard(toolDir.path());

    AuditRunner::RunRequest req;
    req.projectRoot      = projectDir.path();
    req.tools             = {QStringLiteral("mypy")};
    req.capPerToolSeconds = 5;  // AuditRunner::internal::kCapPerToolMin

    const AuditRunner::RunResult r = AuditRunner::runAudit(req);

    ASSERT_TRUE(r.byTool.contains(QStringLiteral("mypy")))
        << "test setup: the fake mypy never resolved/ran — code: "
        << r.code.toStdString() << " error: " << r.error.toStdString();
    EXPECT_EQ(r.byTool.value(QStringLiteral("mypy")).status,
              QStringLiteral("timed_out"))
        << "the per-tool cap must still mark a tool it had to kill "
           "timed_out, not crashed or ok";

    // The script writes the sleeper's pid before it ever blocks, so it is
    // on disk well before the 5s cap can fire.
    QFile pf(pidFile);
    ASSERT_TRUE(pf.open(QIODevice::ReadOnly))
        << "test setup: fake mypy never wrote its sleeper's pid to "
        << pidFile.toStdString();
    bool ok = false;
    const qint64 sleeperPid =
        QString::fromUtf8(pf.readAll()).trimmed().toLongLong(&ok);
    ASSERT_TRUE(ok) << "test setup: pid file did not contain a number";

    const bool gone = ants_test::pollProcessGone(static_cast<pid_t>(sleeperPid));

    // Clean up regardless of outcome — never leak a 300s sleeper into the
    // test host, red or green.
    ::kill(static_cast<pid_t>(sleeperPid), SIGKILL);

    EXPECT_TRUE(gone)
        << "ANTS-5038: fake mypy's sleeper pid " << sleeperPid
        << " was still alive after AuditRunner's per-tool cap killed the "
           "tool — expected: gone (kill(pid,0) fails ESRCH, or the pid is "
           "a zombie); actual: still a live, non-zombie process. The cap's "
           "proc->terminate()/proc->kill() reach only the QProcess's "
           "direct child, not the tool's process group.";
}
