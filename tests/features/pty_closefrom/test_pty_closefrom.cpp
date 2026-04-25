// PTY closefrom — source-grep regression test.
// See spec.md. Fails non-zero if the post-fork close-inherited-FDs
// path reverts to the hard-coded fd<1024 loop, drops close_range(2),
// or skips the RLIMIT_NOFILE-bounded fallback.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_PTY_PATH
#  error "SRC_PTY_PATH compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Locate the body of `if (m_childPid == 0) { ... }` (the post-fork
// child branch). Returns empty string if not found.
static std::string extractChildBranch(const std::string &src) {
    std::regex marker(R"(if\s*\(\s*m_childPid\s*==\s*0\s*\)\s*\{)");
    std::smatch m;
    if (!std::regex_search(src, m, marker)) return {};
    size_t start = m.position(0) + m.length(0);
    // Walk forward, tracking brace depth. Skip braces inside strings
    // and char-literals — for ptyhandler.cpp the simple counter is
    // sufficient because the child branch contains no commented-out
    // braces.
    int depth = 1;
    size_t i = start;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(start, i - start - 1);
}

int main() {
    const std::string src = slurp(SRC_PTY_PATH);
    const std::string childBody = extractChildBranch(src);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    if (childBody.empty()) {
        fail("precondition: could not locate `if (m_childPid == 0) { ... }` "
             "block in src/ptyhandler.cpp — refactor likely renamed the "
             "post-fork child branch and this test needs updating.");
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/pty_closefrom/spec.md for context\n",
            failures);
        return 1;
    }

    // INV-1 — close_range syscall is invoked at the close-inherited-FDs
    // site. Accept the raw `SYS_close_range` token; mechanism may be a
    // ::syscall(SYS_close_range, ...) call or a wrapper guarded by an
    // #ifdef SYS_close_range.
    if (childBody.find("SYS_close_range") == std::string::npos) {
        fail("INV-1: SYS_close_range not referenced inside the post-fork "
             "child branch. Without close_range(2) the only path is the "
             "fallback loop, defeating the speed/correctness gain.");
    }

    // INV-2 — the hard-coded fd<1024 loop is gone. The exact pattern of
    // the pre-fix code is: for (int fd = 3; fd < 1024; ++fd).
    std::regex hardcodedLoop(
        R"(for\s*\(\s*int\s+fd\s*=\s*3\s*;\s*fd\s*<\s*1024\s*;)");
    if (std::regex_search(src, hardcodedLoop)) {
        fail("INV-2: hard-coded `for (int fd = 3; fd < 1024; ...)` loop is "
             "back in src/ptyhandler.cpp. This silently leaks FDs above "
             "1023 on systemd / container default RLIMIT_NOFILE profiles.");
    }

    // INV-3 — fallback consults RLIMIT_NOFILE. Either the symbol or
    // `getrlimit` must appear inside the child branch.
    if (childBody.find("RLIMIT_NOFILE") == std::string::npos &&
        childBody.find("getrlimit") == std::string::npos) {
        fail("INV-3: RLIMIT_NOFILE / getrlimit not referenced inside the "
             "post-fork child branch. Fallback must be bounded by the "
             "runtime soft cap, not a compile-time constant.");
    }

    // INV-4 — required headers included. <sys/resource.h> for getrlimit,
    // <sys/syscall.h> for SYS_close_range. Tolerant of whitespace.
    std::regex resourceInclude(R"(#include\s*<sys/resource\.h>)");
    if (!std::regex_search(src, resourceInclude)) {
        fail("INV-4: #include <sys/resource.h> not found — getrlimit / "
             "RLIMIT_NOFILE may not be declared.");
    }
    std::regex syscallInclude(R"(#include\s*<sys/syscall\.h>)");
    if (!std::regex_search(src, syscallInclude)) {
        fail("INV-4: #include <sys/syscall.h> not found — SYS_close_range "
             "may not be declared.");
    }

    // INV-5 — fallback bound is capped to a sane maximum. Look for any
    // of the common spellings of 65536 within the child body.
    bool capped =
        childBody.find("65536") != std::string::npos ||
        childBody.find("0x10000") != std::string::npos ||
        childBody.find("1 << 16") != std::string::npos ||
        childBody.find("1<<16") != std::string::npos;
    if (!capped) {
        fail("INV-5: no numeric cap (65536 / 0x10000 / 1<<16) found inside "
             "the post-fork child branch. An unbounded RLIMIT_NOFILE-sized "
             "loop on hardened-server profiles (rlim_cur ~ 1M) issues a "
             "million close() syscalls per shell spawn.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/pty_closefrom/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: PTY child closes inherited FDs via close_range "
                "with RLIMIT_NOFILE-bounded fallback\n");
    return 0;
}
