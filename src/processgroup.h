#pragma once

#include <QProcess>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

// ANTS-5038 / ANTS-5063 — a tool run through a shell, or one that forks its
// own helpers (semgrep → semgrep-core, cmake → ninja → compilers), leaves its
// children running when only its own pid is signalled. Stopping it must
// reach everything it started.
namespace ProcessGroup {

// Call before QProcess::start(). The child calls setsid(), so it leads a new
// process group whose id is its pid, and everything it starts joins it.
inline void startsOwnGroup(QProcess &p) {
    p.setChildProcessModifier([] { ::setsid(); });
}

// Send `sig` to the group led by `pid`. Take the pid while the process is
// running: processId() reads 0 once the leader has exited, while helpers it
// started may still be alive. A pid that leads no group is a harmless ESRCH.
inline void signalGroup(qint64 pid, int sig) {
    if (pid > 0) ::kill(-static_cast<pid_t>(pid), sig);
}

inline void signalGroup(const QProcess &p, int sig) {
    signalGroup(p.processId(), sig);
}

// Poll up to `ms` for the group led by `pid` to have no process left.
// Blocking: for worker threads, not the GUI thread.
inline bool waitGroupGone(qint64 pid, int ms) {
    for (int waited = 0;; waited += 50) {
        if (pid <= 0 || (::kill(-static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH))
            return true;
        if (waited >= ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace ProcessGroup
