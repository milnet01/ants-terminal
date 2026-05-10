// Feature-conformance test for spec.md —
//
// Source-grep test. Pty::~Pty must escalate child reaping
// synchronously on the parse-worker thread (not via a detached
// std::thread), with bounded SIGTERM and SIGKILL waits so total
// destructor time stays within m_parseThread->wait(2000)'s budget.
//
// ANTS-1189 (2026-05-08): the previous spec required a detached
// std::thread for the escalation. That design caused reproducible
// SIGABRT crashes in ~TerminalWidget on app exit (heap corruption
// from worker-thread/main-thread teardown race). The current spec
// requires synchronous escalation — the threaded VtStream
// architecture (~Pty already runs on parse-worker) makes the
// detach redundant, and removing it eliminates the crash.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <gtest/gtest.h>

#ifndef SRC_PTYHANDLER_CPP_PATH
#  error "SRC_PTYHANDLER_CPP_PATH compile definition required"
#endif

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const std::string &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label, detail.c_str());
        ++g_failures;
    }
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

// Slice from "Pty::~Pty()" up to the first top-level closing
// brace that ends the destructor.
std::string extractDtor(const std::string &src) {
    const size_t dtorAt = src.find("Pty::~Pty()");
    if (dtorAt == std::string::npos) return {};
    const size_t openBrace = src.find('{', dtorAt);
    if (openBrace == std::string::npos) return {};
    int depth = 1;
    size_t i = openBrace + 1;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        if (c == '"') {
            ++i;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\') ++i;
                if (i < src.size()) ++i;
            }
            if (i < src.size()) ++i;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    return src.substr(openBrace, i - openBrace);
}

}  // namespace

TEST(PtyDtorOffMainThread, Main) {
    const std::string src = slurp(SRC_PTYHANDLER_CPP_PATH);
    const std::string dtor = extractDtor(src);
    expect(!dtor.empty(), "extract/dtor-body-found");

    // I1 — SIGHUP sent up front.
    expect(contains(dtor, "::kill(m_childPid, SIGHUP);"),
           "I1/SIGHUP-sent-up-front");

    // I2 — master FD closed before reap loop.
    {
        const size_t closeAt = dtor.find("::close(m_masterFd);");
        const size_t firstWaitpid = dtor.find("::waitpid(m_childPid");
        expect(closeAt != std::string::npos &&
               firstWaitpid != std::string::npos &&
               closeAt < firstWaitpid,
               "I2/master-FD-closed-before-reap-loop");
    }

    // I3 — cheap WNOHANG reap before escalation.
    {
        const size_t firstWaitpid = dtor.find(
            "::waitpid(m_childPid, &status, WNOHANG)");
        const size_t firstSigterm = dtor.find("SIGTERM");
        expect(firstWaitpid != std::string::npos &&
               firstSigterm != std::string::npos &&
               firstWaitpid < firstSigterm,
               "I3/WNOHANG-cheap-reap-before-SIGTERM");
    }

    // I4 — synchronous SIGTERM/SIGKILL escalation, both bounded.
    expect(contains(dtor, "::kill(m_childPid, SIGTERM);"),
           "I4/SIGTERM-issued");
    expect(contains(dtor, "::kill(m_childPid, SIGKILL);"),
           "I4/SIGKILL-issued");
    {
        // SIGTERM loop: 50 × 10 ms = 500 ms cap.
        std::regex sigtermLoop(
            R"(SIGTERM[\s\S]*?for \(int i = 0; i < 50; \+\+i\)[\s\S]*?usleep\(10000\))");
        expect(std::regex_search(dtor, sigtermLoop),
               "I4/SIGTERM-loop-bounded-50x10ms");
    }
    {
        // SIGKILL reap: 100 × 10 ms = 1 s cap.
        std::regex sigkillLoop(
            R"(SIGKILL[\s\S]*?for \(int i = 0; i < 100; \+\+i\)[\s\S]*?usleep\(10000\))");
        expect(std::regex_search(dtor, sigkillLoop),
               "I4/SIGKILL-reap-bounded-100x10ms");
    }

    // I5 — NO std::thread in the destructor (anti-regression for ANTS-1189).
    expect(!contains(dtor, "std::thread"),
           "I5/no-std::thread-in-dtor");
    expect(!contains(dtor, ".detach("),
           "I5/no-detach-in-dtor");

    // I6 — NO blocking waitpid(pid, _, 0) after SIGKILL.
    {
        // The exact pattern that hung indefinitely in the pre-fix
        // code. Catches both `waitpid(m_childPid, &status, 0)` and
        // any equivalent literal-0 third-arg form.
        std::regex blockingWaitpid(
            R"(::waitpid\([^,]+,\s*&[A-Za-z_]+,\s*0\s*\))");
        expect(!std::regex_search(dtor, blockingWaitpid),
               "I6/no-blocking-waitpid-third-arg-0");
    }

    // I7 — <thread> header not included (companion to I5).
    expect(!contains(src, "#include <thread>"),
           "I7/no-thread-header-include");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        FAIL();
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return;
}

