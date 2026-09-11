// ANTS-5039 — source-scrape regression test. See spec.md.
//
// The `compiler_warnings` audit check (auditdialog.cpp, raw m_checks.append)
// configures and builds the whole project from scratch, inside the default
// 30 s per-check timeout that every other AuditCheck gets unless it opts
// out. A from-scratch configure+build of this project cannot finish in
// 30 s, so the check is killed every time it is run; the SIGKILL (since
// ANTS-5038, sent to the whole process group after a SIGTERM grace period)
// arrives before the trailing `rm -rf "$tmpdir"` ever runs, so a fresh
// `mktemp -d` build tree — and the orphaned cmake/ninja/compiler processes
// still writing into it — is left behind in /tmp on every attempt.
//
// Four invariants, three of them source-scrape and expected to fail
// against the current tree, one behavioural and expected to already pass
// (the `[ -f CMakeLists.txt ]` guard is not what ANTS-5039 is about).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>

#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// ---- INV-1 helper: largest integer (or `a * b * c` product of adjacent
// integer literals) anywhere in the block, evaluated without a full
// expression parser. Deliberately threshold-based rather than
// field-position-based: the block's other numeric literals (the
// OutputFilter's `maxLines` field, the `nproc`-derived arithmetic inside
// the command string) are all well under a minutes-scale threshold, so a
// plain "biggest number anywhere" scan can't be fooled by them, and it
// doesn't have to know whether a fix expresses the new timeout as a bare
// literal (`1200000`) or as an expression (`20 * 60 * 1000`).
long long largestIntOrProduct(const std::string &block) {
    long long best = 0;
    const std::size_t n = block.size();
    std::size_t i = 0;
    while (i < n) {
        if (!std::isdigit(static_cast<unsigned char>(block[i]))) {
            ++i;
            continue;
        }
        long long product = 1;
        std::size_t j = i;
        while (true) {
            std::size_t k = j;
            while (k < n && std::isdigit(static_cast<unsigned char>(block[k]))) ++k;
            product *= std::stoll(block.substr(j, k - j));
            // Look past optional whitespace for a '*' that continues the
            // same multiplicative expression.
            std::size_t p = k;
            while (p < n && std::isspace(static_cast<unsigned char>(block[p]))) ++p;
            if (p < n && block[p] == '*') {
                ++p;
                while (p < n && std::isspace(static_cast<unsigned char>(block[p]))) ++p;
                if (p < n && std::isdigit(static_cast<unsigned char>(block[p]))) {
                    j = p;
                    continue;
                }
            }
            i = k;
            break;
        }
        best = std::max(best, product);
    }
    return best;
}

// ---- INV-4 helper: pull the `command` field out of the block. The
// arguments are a plain, unescaped-except-for-\"-and-\\, adjacent-literal
// run — id, name, description, category, command, CheckType::..., ... — so
// this walks the first four literal fields (skipping them) and then reads
// the run of literals that makes up the fifth.

std::size_t skipWs(const std::string &s, std::size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

// Decodes exactly one "..." literal starting at s[pos]. Only \" and \\ are
// unescaped, per this test's stated extraction scope; any other escape is
// passed through as-is (the command string in this block uses only those
// two). Returns {text, pos-after-closing-quote, ok}.
struct Lit { std::string text; std::size_t next; bool ok; };

Lit readOneLiteral(const std::string &s, std::size_t pos) {
    if (pos >= s.size() || s[pos] != '"') return {"", pos, false};
    std::string out;
    std::size_t i = pos + 1;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size() &&
            (s[i + 1] == '"' || s[i + 1] == '\\')) {
            out.push_back(s[i + 1]);
            i += 2;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    if (i >= s.size()) return {"", pos, false};  // unterminated
    return {out, i + 1, true};
}

// Reads a run of one-or-more adjacent literals (whitespace-only between
// them, C++ literal concatenation) and concatenates their decoded text.
Lit readLiteralRun(const std::string &s, std::size_t pos) {
    std::string out;
    bool any = false;
    while (true) {
        const std::size_t p = skipWs(s, pos);
        if (p >= s.size() || s[p] != '"') { pos = p; break; }
        const Lit one = readOneLiteral(s, p);
        if (!one.ok) return {out, p, false};
        out += one.text;
        pos = one.next;
        any = true;
    }
    return {out, pos, any};
}

bool skipFieldAndComma(const std::string &s, std::size_t &pos) {
    const Lit run = readLiteralRun(s, pos);
    if (!run.ok) return false;
    const std::size_t p = skipWs(s, run.next);
    if (p >= s.size() || s[p] != ',') return false;
    pos = p + 1;
    return true;
}

// Extracts the 5th positional literal field (command), skipping id, name,
// description, category. `ok` is false when the shape isn't the plain
// adjacent-literal-run this test can safely decode — callers must skip
// rather than assert on a possibly-wrong string.
std::string extractCommand(const std::string &block, bool &ok) {
    ok = false;
    std::size_t pos = block.find('"');
    if (pos == std::string::npos) return {};
    for (int field = 0; field < 4; ++field) {
        if (!skipFieldAndComma(block, pos)) return {};
    }
    const Lit cmd = readLiteralRun(block, pos);
    if (!cmd.ok) return {};
    ok = true;
    return cmd.text;
}

// ---- INV-4 helper: run `bash -c command` with cwd = dir, no shell
// in between (execl takes the command string directly as argv[2], exactly
// how src/auditdialog.cpp's m_process->start("/bin/bash", {"-c",
// check.command}) invokes it) so no extra layer of quoting can corrupt the
// extracted string. Bounded by a wall-clock timeout so a mis-extracted or
// mis-guarded command can't hang the test bundle.
struct RunResult { int exitCode; std::string output; bool completed; };

RunResult runBashC(const std::string &command, const std::string &cwd,
                    int timeoutSec) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1, "", false};

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {-1, "", false};
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (chdir(cwd.c_str()) != 0) _exit(127);
        execl("/bin/bash", "bash", "-c", command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);

    std::string output;
    char buf[4096];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    bool timedOut = false;
    while (true) {
        const auto remain = deadline - std::chrono::steady_clock::now();
        if (remain <= std::chrono::steady_clock::duration::zero()) {
            timedOut = true;
            break;
        }
        const auto remainMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(remain).count();
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(remainMs / 1000);
        tv.tv_usec = static_cast<suseconds_t>((remainMs % 1000) * 1000);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(pipefd[0], &fds);
        const int rv = select(pipefd[0] + 1, &fds, nullptr, nullptr, &tv);
        if (rv <= 0) { timedOut = (rv == 0); break; }
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        output.append(buf, static_cast<std::size_t>(n));
    }
    close(pipefd[0]);

    if (timedOut) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return {-1, output, false};
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exitCode, output, true};
}

}  // namespace

TEST(AuditCompilerWarningsCheck, ExplicitLongTimeout) {
    // INV-1
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string stripped = ants_test::stripComments(src);
    const std::string block =
        ants_test::regionBetween(stripped, "\"compiler_warnings\"", "});");
    ASSERT_FALSE(block.empty())
        << "compiler_warnings m_checks.append block not found in "
           "auditdialog.cpp — the source shape changed under this test";

    constexpr long long kMinTimeoutMs = 180000;  // 3 minutes
    const long long largest = largestIntOrProduct(block);
    EXPECT_GE(largest, kMinTimeoutMs)
        << "ANTS-5039: no integer literal (or a*b*c product of adjacent "
           "integer literals) in the compiler_warnings block reaches "
        << kMinTimeoutMs
        << " (3 minutes) — expected: an explicit timeoutMs of several "
           "minutes for a from-scratch configure+build; actual: largest "
           "value found was " << largest
        << ", so the check still runs under AuditCheck::timeoutMs's 30000 ms "
           "default and is killed before it can finish.";
}

TEST(AuditCompilerWarningsCheck, TrapCleansUpScratchDirOnExit) {
    // INV-2
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string stripped = ants_test::stripComments(src);
    const std::string block =
        ants_test::regionBetween(stripped, "\"compiler_warnings\"", "});");
    ASSERT_FALSE(block.empty());

    const std::size_t trapPos = block.find("trap");
    EXPECT_NE(trapPos, std::string::npos)
        << "ANTS-5039: no 'trap' in the compiler_warnings command — "
           "expected: a `trap ... EXIT` that removes the scratch build "
           "dir even when the check is killed by its timeout or cancelled; "
           "actual: no 'trap' token found in the command.";
    if (trapPos == std::string::npos) return;

    const std::size_t exitPos = block.find("EXIT", trapPos);
    EXPECT_NE(exitPos, std::string::npos)
        << "ANTS-5039: found 'trap' but no 'EXIT' after it — expected a "
           "trap registered on EXIT; actual context: \""
        << block.substr(trapPos, 120) << "\"";
    if (exitPos == std::string::npos) return;

    const std::string span = block.substr(trapPos, exitPos - trapPos);
    EXPECT_NE(span.find("rm"), std::string::npos)
        << "ANTS-5039: the trap...EXIT clause doesn't call `rm` — expected "
           "the trap body to remove the scratch dir; actual span: \""
        << span << "\"";
}

TEST(AuditCompilerWarningsCheck, NoMktempDForBuildDir) {
    // INV-3
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string stripped = ants_test::stripComments(src);
    const std::string block =
        ants_test::regionBetween(stripped, "\"compiler_warnings\"", "});");
    ASSERT_FALSE(block.empty());

    const std::size_t pos = block.find("mktemp -d");
    EXPECT_EQ(pos, std::string::npos)
        << "ANTS-5039: compiler_warnings still creates its build dir with "
           "`mktemp -d` at block offset " << pos
        << " — expected: one FIXED per-user scratch directory under "
           "${TMPDIR:-/tmp}, removed before the run starts and again on "
           "exit, so a run a timeout kills leaves at most one tree for the "
           "next run to clear; actual: `mktemp -d` present, so every run "
           "(and every kill) mints a fresh, never-reclaimed directory name.";
}

TEST(AuditCompilerWarningsCheck, NoCMakeListsGuardIsANoOp) {
    // INV-4 — behavioural, only if the command extracts cleanly.
    const std::string src = ants_test::slurpFile(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string stripped = ants_test::stripComments(src);
    const std::string block =
        ants_test::regionBetween(stripped, "\"compiler_warnings\"", "});");
    ASSERT_FALSE(block.empty());

    bool ok = false;
    const std::string command = extractCommand(block, ok);
    if (!ok) {
        GTEST_SKIP() << "ANTS-5039 INV-4: command field did not extract as "
                        "a clean run of adjacent string literals — skipping "
                        "the behavioural check rather than asserting on a "
                        "possibly-wrong string.";
    }

    char dirTemplate[] = "/tmp/ants_audit_cw_test_XXXXXX";
    const char *projDir = mkdtemp(dirTemplate);
    ASSERT_NE(projDir, nullptr) << "mkdtemp failed: " << std::strerror(errno);

    const RunResult result = runBashC(command, projDir, /*timeoutSec=*/15);
    rmdir(projDir);

    ASSERT_TRUE(result.completed)
        << "ANTS-5039 INV-4: compiler_warnings command did not exit within "
           "15s in a directory with no CMakeLists.txt — expected the "
           "`[ -f CMakeLists.txt ]` guard to make this a fast no-op; "
           "actual: timed out. Output so far: \"" << result.output << "\"";
    EXPECT_EQ(result.exitCode, 0)
        << "ANTS-5039 INV-4: compiler_warnings command exited "
        << result.exitCode << " in a directory with no CMakeLists.txt — "
           "expected: 0 (the guard is a no-op); actual output: \""
        << result.output << "\"";
    EXPECT_TRUE(result.output.empty())
        << "ANTS-5039 INV-4: compiler_warnings command printed output "
           "with no CMakeLists.txt present — expected: nothing; actual: \""
        << result.output << "\"";
}
