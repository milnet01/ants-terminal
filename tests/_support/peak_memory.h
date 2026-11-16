// How far a piece of work raises the process's peak memory, measured in a
// forked child (ANTS-6502, ANTS-6503).
//
// The child's high-water mark starts at its size at the fork, so an earlier
// test's peak cannot hide this one's. Under ASan the number measures the
// allocator's quarantine, which keeps freed blocks resident, so a caller
// skips there: ANTS_TEST_SKIP_UNDER_ASAN().

#pragma once

#include <algorithm>
#include <functional>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ants_test {

// Growth in MiB, capped at 250. -1: the fork failed. 251: `work` returned false.
inline int peakGrowthMiB(const std::function<bool()> &work) {
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        rusage before{}, after{};
        getrusage(RUSAGE_SELF, &before);
        const bool ok = work();
        getrusage(RUSAGE_SELF, &after);
        _exit(ok ? int(std::min<long>(250, (after.ru_maxrss - before.ru_maxrss) / 1024))
                 : 251);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace ants_test

#if defined(__SANITIZE_ADDRESS__)
#define ANTS_TEST_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ANTS_TEST_UNDER_ASAN 1
#endif
#endif

#ifdef ANTS_TEST_UNDER_ASAN
#define ANTS_TEST_SKIP_UNDER_ASAN() \
    GTEST_SKIP() << "ASan keeps freed blocks resident, so the peak measures its quarantine"
#else
#define ANTS_TEST_SKIP_UNDER_ASAN() static_cast<void>(0)
#endif
