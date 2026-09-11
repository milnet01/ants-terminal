// tests/_support/procwait.h — ANTS-5038 helpers.
//
// Has a subprocess actually exited — one that is NOT a direct child of this
// test binary, and in particular not the direct child of anything this test
// itself started? A signal delivered to the wrong target (the process-group
// leader, say, instead of the whole group) leaves the process a test cares
// about running while every QProcess-level accessor the test could ask ("is
// my child done?") reports success, because the test's own child is what
// actually received the signal. These helpers reach past that and ask the
// kernel about an arbitrary pid directly.
//
// Header-only; no link-time dependency. Included from any test_*.cpp that
// already links Qt6::Core.

#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <thread>

namespace ants_test {

// True while `pid` is still a live process this user can see: kill(pid, 0)
// succeeding, or failing with anything other than ESRCH (e.g. EPERM, which
// this test never expects but which must not be misread as "gone").
inline bool pidAlive(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno != ESRCH;
}

// A zombie (state 'Z' in /proc/<pid>/stat) is a process the kernel has
// already torn down to just an exit code, waiting on its parent to reap it —
// for these tests that counts as "gone": nothing is left running or holding
// memory. Returns false (not a zombie) on any read failure, including the
// pid no longer existing at all — pidAlive() is what answers that case.
inline bool pidIsZombie(pid_t pid) {
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray line = f.readLine();
    // Format: "<pid> (<comm>) <state> ...". <comm> may itself contain
    // spaces or parens, so anchor on the LAST ')' rather than the first.
    const int close = line.lastIndexOf(')');
    if (close < 0 || close + 2 >= line.size()) return false;
    return line.at(close + 2) == 'Z';
}

// Poll up to `totalMs` (default ~2s) for `pid` to become unreachable or a
// zombie. Returns true the moment either holds; false if it is still a
// live, non-zombie process when the budget runs out.
inline bool pollProcessGone(pid_t pid, int totalMs = 2000, int stepMs = 50) {
    for (int waited = 0; waited <= totalMs; waited += stepMs) {
        if (!pidAlive(pid) || pidIsZombie(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    return !pidAlive(pid) || pidIsZombie(pid);
}

}  // namespace ants_test
