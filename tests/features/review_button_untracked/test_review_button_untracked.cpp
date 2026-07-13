// Feature-conformance test for spec.md —
//
// Behavioural test of ants::parseReviewPorcelain (the Review Changes
// button predicate) plus a source-grep guard that the call site uses
// it. ANTS-1874.

#include "../../_support/expect.h"

#include "reviewbuttonstate.h"

#include <cstdlib>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

TEST(ReviewButtonUntracked, Main) {
    expect_reset();
    using ants::parseReviewPorcelain;

    // I1 — untracked-only output is dirty (the bug this fixes).
    {
        auto st = parseReviewPorcelain(
            "## main...origin/main\n?? newfile.cpp\n");
        expect(st.dirty, "I1/untracked-only-is-dirty");
        expect(!st.ahead, "I1/untracked-only-not-ahead");
    }

    // I2 — clean repo: neither dirty nor ahead.
    {
        auto st = parseReviewPorcelain("## main...origin/main\n");
        expect(!st.dirty, "I2/clean-not-dirty");
        expect(!st.ahead, "I2/clean-not-ahead");
    }

    // I3 — tracked modification stays dirty.
    {
        auto st = parseReviewPorcelain(
            "## main...origin/main\n M tracked.cpp\n");
        expect(st.dirty, "I3/tracked-modified-is-dirty");
    }

    // I4 — ahead detection survives the extraction.
    {
        expect(parseReviewPorcelain(
                   "## main...origin/main [ahead 2]\n").ahead,
               "I4/ahead-only");
        expect(parseReviewPorcelain(
                   "## main...origin/main [ahead 1, behind 3]\n").ahead,
               "I4/ahead-and-behind");
        expect(!parseReviewPorcelain(
                   "## main...origin/main [behind 3]\n").ahead,
               "I4/behind-only-not-ahead");
    }

    // I5 — call site uses the helper and dropped the carve-out.
    {
        const std::string src = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
        expect(contains(src, "parseReviewPorcelain"),
               "I5/callsite-uses-helper");
        expect(!contains(src, "ln.startsWith(\"?? \")"),
               "I5/carve-out-removed");
    }
}
