// ANTS-5038 — behavioural regression test for ProcessGroup::startsOwnGroup /
// ProcessGroup::signalGroup (src/processgroup.h). See spec.md INV-1.
//
// Why this exists: AuditRunner and AuditDialog stop a tool by signalling
// only the QProcess's own pid. A tool that forks a helper (semgrep spawns
// semgrep-core; a shell pipeline forks whatever it forks) leaves that
// helper running after the "stopped" tool is gone. The fix is process-group
// signalling; this test drives the seam directly, independent of either
// call site, and shows a forked grandchild is still alive when only the
// group leader is signalled.

#include <gtest/gtest.h>
#include "processgroup.h"
#include "../../_support/procwait.h"

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <csignal>

TEST(ProcessGroupHelper, SignalGroupReachesTheGrandchild) {
    QProcess p;
    ProcessGroup::startsOwnGroup(p);
    p.start(QStringLiteral("/bin/sh"),
            {QStringLiteral("-c"),
             QStringLiteral("sleep 30 & echo $!; wait")});
    ASSERT_TRUE(p.waitForStarted(2000))
        << "test setup: /bin/sh failed to start";
    ASSERT_TRUE(p.waitForReadyRead(2000))
        << "test setup: no grandchild pid line arrived from /bin/sh";

    bool ok = false;
    const qint64 grandchildPid = p.readLine().trimmed().toLongLong(&ok);
    ASSERT_TRUE(ok) << "test setup: could not parse the grandchild pid";
    ASSERT_TRUE(ants_test::pidAlive(static_cast<pid_t>(grandchildPid)))
        << "test setup: grandchild pid " << grandchildPid
        << " was not alive right after it printed its own pid";

    ProcessGroup::signalGroup(p, SIGKILL);
    p.kill();
    p.waitForFinished(2000);

    const bool gone =
        ants_test::pollProcessGone(static_cast<pid_t>(grandchildPid));

    // Clean up regardless of outcome — never leak a 30s sleeper into the
    // test host, red or green.
    ::kill(static_cast<pid_t>(grandchildPid), SIGKILL);

    EXPECT_TRUE(gone)
        << "ANTS-5038: grandchild pid " << grandchildPid
        << " (a `sleep 30` forked by the shell ProcessGroup::startsOwnGroup "
           "started) was still alive after signalGroup(SIGKILL) + kill() — "
           "expected: gone (kill(pid,0) fails ESRCH, or the pid is a "
           "zombie); actual: still a live, non-zombie process. "
           "signalGroup is signalling only the group leader, not the "
           "process group.";
}
