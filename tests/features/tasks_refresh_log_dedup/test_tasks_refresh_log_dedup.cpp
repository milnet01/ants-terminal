// Source-grep harness for ANTS-1859 — pins the tasks/refresh debug
// dedup key in ClaudeStatusBarController::refreshTasksButton. The key
// must exclude the transcript mtime (Claude streams output into the
// JSONL, ticking the mtime every poll), so the line logs only on a
// real task-state transition. See spec.md.
//
// Exit 0 = all 4 invariants hold.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

}  // namespace

static int runMain() {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);

    int failures = 0;
    auto inv = [&](int n, bool ok, const char *why) {
        if (!ok) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "INV-%d", n);
            failures += fail(buf, why);
        }
    };

    // Isolate the tasks-path refresh slot, then its dedup signature.
    // (refreshBgTasksButton has its own, correctly-keyed sig earlier in
    // the file — we want the tasks one specifically.)
    const std::size_t methodPos =
        src.find("void ClaudeStatusBarController::refreshTasksButton");
    if (methodPos == std::string::npos)
        fail("setup", "refreshTasksButton definition not found");
    const std::size_t sigPos = src.find("const QString sig =", methodPos);
    if (sigPos == std::string::npos)
        fail("setup", "tasks sig construction not found");
    const std::size_t sigEnd = src.find(';', sigPos);
    if (sigEnd == std::string::npos)
        fail("setup", "tasks sig statement has no terminator");
    const std::string sigBlock = src.substr(sigPos, sigEnd - sigPos);

    // INV-1 — the dedup key excludes the transcript mtime.
    inv(1, !contains(sigBlock, "preMtimeMs"),
        "tasks dedup signature must not key on preMtimeMs (ANTS-1859) — "
        "the transcript mtime ticks every poll and defeats the dedup");

    // INV-2 — the key still keys on the task state + visibility branch,
    // so dedup is not over-broadened into swallowing real transitions.
    inv(2,
        contains(sigBlock, ".arg(total)") &&
            contains(sigBlock, ".arg(unfinished)") &&
            contains(sigBlock, ".arg(inProgress)") &&
            contains(sigBlock, ".arg(pending)") &&
            contains(sigBlock, ".arg(done)") &&
            contains(sigBlock, "branch"),
        "tasks dedup signature must still key on total/unfinished/"
        "inProgress/pending/done + branch");

    // INV-3 — mtime stays in the logged LINE (the ANTS-1458 latency
    // column), just not in the dedup key. The only preMtimeMs reference
    // after the sig statement is the ANTS_LOG argument.
    inv(3, src.find("preMtimeMs", sigEnd) != std::string::npos,
        "preMtimeMs must still be emitted in the ANTS_LOG line "
        "(latency context preserved on genuine changes)");

    // INV-4 — exactly one field (mtime) was removed: the outer format
    // string is 7 placeholders, not the pre-fix 8.
    std::smatch m;
    const std::string region = src.substr(methodPos);
    std::regex fmtRe(R"(const QString sig = QStringLiteral\(\"([^\"]*)\"\))");
    if (std::regex_search(region, m, fmtRe)) {
        inv(4, m[1].str() == "%1|%2|%3|%4|%5|%6|%7",
            "tasks sig format string must be 7 placeholders "
            "(%1..%7); found a different width");
    } else {
        inv(4, false, "could not locate the tasks sig format literal");
    }

    return failures == 0 ? 0 : 1;
}

TEST(TasksRefreshLogDedup, Main) { ASSERT_EQ(0, runMain()); }
