// tests/_support/bounded_child.h — run a call that may block forever.
//
// For a test whose pre-fix code blocks (opening a FIFO, a loop that never
// ends): the call runs in a forked child, so the red run fails at a deadline
// instead of hanging the suite. Build what the call needs before calling this;
// the child only runs `body`, and must not use gtest's EXPECT/ASSERT, whose
// results stay in the child.
//
// Header-only; no link-time dependency.

#pragma once

#include <chrono>
#include <csignal>
#include <functional>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ants_test {

// The child's exit code: `body`'s return value, kept to 0-255. -1 when the
// child was still running at `timeoutMs` (it is then killed), -2 when fork
// failed, -3 when the child died on a signal.
inline int runInChildWithin(int timeoutMs, const std::function<int()> &body) {
    const pid_t pid = ::fork();
    if (pid < 0) return -2;
    if (pid == 0) ::_exit(body() & 0xff);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return -1;
}

}  // namespace ants_test
