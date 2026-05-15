// Feature-conformance regression for ANTS-1396.
// See tests/features/terminal_for_caller_isolation/spec.md.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Find the function body for MainWindow::terminalForCaller; return
// the [open-brace, matching-close-brace) substring. Naive matching
// — works because the body has no anonymous namespaces or lambdas
// with extra braces beyond the for-loop block.
std::string functionBody(const std::string &src) {
    const auto sig = src.find("MainWindow::terminalForCaller");
    if (sig == std::string::npos) return {};
    const auto open = src.find('{', sig);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return src.substr(open, i - open + 1);
        }
    }
    return {};
}

}  // namespace

// INV-1 — empty callerCwd → focusedTerminal() back-compat.
TEST(TerminalForCallerIsolation, EmptyCallerCwdFallsBackToFocused) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    const std::string body = functionBody(mw);
    ASSERT_FALSE(body.empty()) << "terminalForCaller body not located";
    // The empty-callerCwd path returns focusedTerminal(). Look for
    // the literal "callerCwd.isEmpty()" branch and the focused call.
    EXPECT_NE(body.find("callerCwd.isEmpty()"), std::string::npos)
        << "ANTS-1396 INV-1: empty-callerCwd branch missing";
    EXPECT_NE(body.find("focusedTerminal()"), std::string::npos)
        << "ANTS-1396 INV-1: focused fallback missing for empty callerCwd";
}

// INV-3 — non-empty + no-match → nullptr (NOT focusedTerminal).
TEST(TerminalForCallerIsolation, NoMatchReturnsNullptr) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    const std::string body = functionBody(mw);
    ASSERT_FALSE(body.empty());
    // Explicit nullptr return must be present.
    EXPECT_NE(body.find("return nullptr;"), std::string::npos)
        << "ANTS-1396 INV-3: explicit `return nullptr;` missing — "
           "the no-match case must NOT silently degrade to "
           "focusedTerminal()";
}

// INV-3 regression-lock — exactly one focusedTerminal() call,
// guarded by the empty-callerCwd branch. v1 had the focused fallback
// as the function's terminal `return` (unconditional); v2 has it
// only inside the `if (callerCwd.isEmpty())` arm.
TEST(TerminalForCallerIsolation, FocusedFallbackIsOnlyForEmptyCaller) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    const std::string body = functionBody(mw);
    ASSERT_FALSE(body.empty());
    int focusedCount = 0;
    size_t pos = 0;
    while ((pos = body.find("focusedTerminal()", pos)) != std::string::npos) {
        ++focusedCount;
        ++pos;
    }
    EXPECT_EQ(focusedCount, 1)
        << "ANTS-1396 INV-3: terminalForCaller must call "
           "focusedTerminal() exactly once (only in the empty-"
           "callerCwd back-compat branch). Multiple call sites "
           "suggest the v1 cross-project leak has crept back.";
}

// INV-4 — unresolvable canonical path → nullptr (not focused).
TEST(TerminalForCallerIsolation, UnresolvableCanonicalReturnsNullptr) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    const std::string body = functionBody(mw);
    ASSERT_FALSE(body.empty());
    // The wantCanonical-empty branch should reach a `return nullptr;`,
    // not fall through to focusedTerminal(). Verify the branch text.
    EXPECT_NE(body.find("wantCanonical.isEmpty()"), std::string::npos)
        << "ANTS-1396 INV-4: missing wantCanonical.isEmpty() guard";
}
