// Source-grep harness for ANTS-1246 — pins the Tasks chip's
// progress semantics (☰ <done>/<total>, hide at done >= total,
// visible during in-progress runs). See spec.md.
//
// Exit 0 = all 4 invariants hold.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

static int runMain() {
    const std::string src = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);

    int failures = 0;
    auto inv = [&](int n, bool ok, const char *why) {
        if (!ok) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "INV-%d", n);
            failures += fail(buf, why);
        }
    };

    // INV-1 — chip text reads `☰ <done>/<total>`. We pin two
    // adjacent pieces of evidence: the format literal and the
    // arg(done).arg(total) call.
    std::regex chipText(
        R"(setText\([\s\S]{0,40}"\xe2\x98\xb0 %1/%2"[\s\S]{0,80}\.arg\(done\)\.arg\(total\))");
    inv(1, std::regex_search(src, chipText),
        "refreshTasksButton must call setText with \"☰ %1/%2\" and "
        ".arg(done).arg(total) (chip text format = done/total)");

    // INV-2 — hide-gate predicate is `done >= total`.
    inv(2, contains(src, "done >= total"),
        "hide gate must use `done >= total` predicate (ANTS-1246 "
        "all-completion semantics)");

    // INV-3 — anti-regression: the deleted `unfinished <= 0`
    // predicate must NOT reappear. The variable `unfinished` may
    // still appear in scope (it's still computed for the diagnostic
    // log), but a *gate* of the form `unfinished <= 0` would
    // re-introduce the bug.
    inv(3, !contains(src, "unfinished <= 0"),
        "stale predicate `unfinished <= 0` is back — that re-breaks "
        "ANTS-1246's visibility contract (chip vanished while "
        "in_progress tasks remained, per the 2026-05-12 user report)");

    // INV-4 — tooltip's done/running/outstanding breakdown intact.
    inv(4, contains(src, "%3 done"),
        "tooltip lost its `%3 done` slot — informational breakdown "
        "regressed");
    inv(4, contains(src, "%4 running"),
        "tooltip lost its `%4 running` slot");
    inv(4, contains(src, "%5 outstanding"),
        "tooltip lost its `%5 outstanding` slot");

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see ANTS-1246 spec for context\n",
            failures);
        return 1;
    }
    std::printf("OK: 4/4 Tasks-chip progress-semantics invariants present\n");
    return 0;
}

TEST(TasksChipDoneOverTotal, Main) {
    ASSERT_EQ(0, runMain());
}
