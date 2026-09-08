// Why this exists: a wide character's continuation cell was adding a
// third cell width to a background run its lead cell already covered,
// so a coloured run spilled one column right per wide char. See spec.md.
//
// Source-grep: paintEvent needs a real paint device and a font with true
// double-width metrics to check in pixels; that is the E2E harness's job.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_TERMINALWIDGET_PATH
#  error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif

namespace {

// The background-run branch: from the extend test to the flush-and-start
// `else` that follows it. Scoping to this slice matters — an isWideCont
// test anywhere else in paintEvent would satisfy a whole-function grep.
std::string extendBranch(const std::string &src) {
    const size_t open = src.find("if (wantBgFill && bgRunWidth > 0");
    if (open == std::string::npos) return {};
    const size_t brace = src.find('{', open);
    if (brace == std::string::npos) return {};
    size_t i = brace + 1;
    int depth = 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(brace + 1, i - brace - 2);
}

}  // namespace

TEST(WideCharBgRunWidth, Main) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALWIDGET_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string extend = extendBranch(src);
    if (extend.empty()) {
        fail("precondition: the background-run extend branch "
             "(`if (wantBgFill && bgRunWidth > 0 …`) was not found in "
             "src/terminalwidget.cpp.");
    } else {
        // INV-1 — the extend branch skips a continuation cell.
        if (extend.find("isWideCont") == std::string::npos) {
            fail("INV-1: the background-run extend branch adds width "
                 "without testing isWideCont. A wide character's lead cell "
                 "already contributes both columns, so the continuation "
                 "cell makes it three cell widths for a two-column glyph — "
                 "and the overshoot accumulates, one column per wide char, "
                 "so a selection over CJK or emoji spills past its text.");
        }
        // The branch should still add the width it computed.
        if (extend.find("bgRunWidth +=") == std::string::npos) {
            fail("INV-1: the extend branch no longer adds cellDrawWidth at "
                 "all — a run can never grow past its first cell.");
        }
    }

    // INV-2 — the start-a-new-run branch is NOT gated on isWideCont. That
    // branch is how a split selection paints the right half of a glyph.
    const std::regex startBranch(
        R"(if\s*\(wantBgFill\)\s*\{\s*(?:[^}]*?)bgRunStartX\s*=)");
    std::smatch m;
    if (!std::regex_search(src, m, startBranch)) {
        fail("precondition: the branch that opens a new background run "
             "was not found.");
    } else if (m.str(0).find("isWideCont") != std::string::npos) {
        fail("INV-2: the branch that OPENS a background run is gated on "
             "isWideCont. A selection boundary falling between the two "
             "halves of a wide character then paints nothing for the right "
             "half; that fill is intended.");
    }

    // INV-3 — the lead cell still contributes double.
    const std::regex leadDouble(
        R"(isWideChar\s*\?\s*m_cellWidth\s*\*\s*2\s*:\s*m_cellWidth)");
    if (!std::regex_search(src, leadDouble)) {
        fail("INV-3: the double-width computation for a wide char's lead "
             "cell is gone. The fix is about the continuation cell, not "
             "about narrowing wide glyphs.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/wide_char_bg_run_width/spec.md\n",
            failures);
    }
    ASSERT_EQ(0, failures);
}
