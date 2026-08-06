// Feature-conformance regression for ANTS-1396.
// See tests/features/terminal_for_caller_isolation/spec.md.
//
// ANTS-1401 refactor (2026-05-16): the four-case decision tree now
// lives in the free function `ants::resolveCallerCwdRoot` defined in
// remotecontrol.cpp. `MainWindow::terminalForCaller` is a thin
// dispatcher that maps `ResolvedRoot::Source` back to a TerminalWidget *.
// The source-scrape assertions below now point at the helper body
// where the literals actually live; the wrapper is covered by an
// additional delegation test that ensures terminalForCaller cannot
// drift from the helper.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {


// ANTS-1597 — read each large source file once and share across TESTs
// rather than re-slurping per case.
const std::string &rcSource() {
    static const std::string s =
        ants_test::slurpRemoteControl();
    return s;
}
const std::string &mwSource() {
    static const std::string s =
        ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    return s;
}

// Find the body for a function whose signature contains the given
// marker; return the [open-brace, matching-close-brace) substring.
// Naive matching — works for our targets (no anon-namespace braces
// inside the helper or the dispatcher).
// ANTS-1468 — delegate to the shared string/comment-aware extractor.
std::string functionBody(const std::string &src, const char *marker) {
    return ants_test::slurpFunctionBody(src, marker);
}

}  // namespace

// INV-1 — empty callerCwd → focusedTerminal() back-compat. The
// canonicalisation + tab walk live in the helper now; the wrapper
// just maps Source::EmptyFallback → focusedTerminal().
TEST(TerminalForCallerIsolation, EmptyCallerCwdFallsBackToFocused) {
    const std::string &rc = rcSource();
    ASSERT_FALSE(rc.empty());
    const std::string body =
        functionBody(rc, "resolveCallerCwdRoot(const MainWindow *main,");
    ASSERT_FALSE(body.empty())
        << "ANTS-1401: resolveCallerCwdRoot body not located";
    EXPECT_NE(body.find("callerCwd.isEmpty()"), std::string::npos)
        << "ANTS-1396 INV-1: empty-callerCwd branch missing from helper";
    EXPECT_NE(body.find("focusedTerminal()"), std::string::npos)
        << "ANTS-1396 INV-1: focused fallback missing for empty callerCwd";
}

// INV-3 — non-empty + no-match → NoMatch (helper) → nullptr (wrapper).
// The helper must produce Source::NoMatch (NOT silently fall through
// to ExplicitMatch with focused-fallback).
TEST(TerminalForCallerIsolation, NoMatchReturnsNullptr) {
    const std::string &rc = rcSource();
    ASSERT_FALSE(rc.empty());
    const std::string body =
        functionBody(rc, "resolveCallerCwdRoot(const MainWindow *main,");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("Source::NoMatch"), std::string::npos)
        << "ANTS-1396 INV-3: explicit NoMatch case missing from helper — "
           "the no-match case must NOT silently degrade to "
           "focusedTerminal() / ExplicitMatch";
}

// INV-3 regression-lock — exactly one focusedTerminal() call in the
// helper (only in the empty-callerCwd back-compat branch). The wrapper
// has one too; both being capped at one is the structural defence.
TEST(TerminalForCallerIsolation, FocusedFallbackIsOnlyForEmptyCaller) {
    const std::string &rc = rcSource();
    ASSERT_FALSE(rc.empty());
    const std::string helperBody =
        functionBody(rc, "resolveCallerCwdRoot(const MainWindow *main,");
    ASSERT_FALSE(helperBody.empty());
    int helperCount = 0;
    {
        size_t pos = 0;
        while ((pos = helperBody.find("focusedTerminal()", pos)) !=
               std::string::npos) {
            ++helperCount;
            ++pos;
        }
    }
    EXPECT_EQ(helperCount, 1)
        << "ANTS-1396 INV-3: resolveCallerCwdRoot must call "
           "focusedTerminal() exactly once (only in the empty-"
           "callerCwd back-compat branch). Multiple call sites "
           "suggest the v1 cross-project leak has crept back.";

    const std::string &mw = mwSource();
    ASSERT_FALSE(mw.empty());
    const std::string wrapperBody =
        functionBody(mw, "MainWindow::terminalForCaller");
    ASSERT_FALSE(wrapperBody.empty());
    int wrapperCount = 0;
    {
        size_t pos = 0;
        while ((pos = wrapperBody.find("focusedTerminal()", pos)) !=
               std::string::npos) {
            ++wrapperCount;
            ++pos;
        }
    }
    EXPECT_EQ(wrapperCount, 1)
        << "ANTS-1396 INV-3 (wrapper): terminalForCaller must call "
           "focusedTerminal() exactly once (only on Source::EmptyFallback).";
}

// INV-4 — unresolvable canonical path → Unresolvable (helper) →
// nullptr (wrapper). Helper must NOT fall back to focused on an
// invalid path.
TEST(TerminalForCallerIsolation, UnresolvableCanonicalReturnsNullptr) {
    const std::string &rc = rcSource();
    ASSERT_FALSE(rc.empty());
    const std::string body =
        functionBody(rc, "resolveCallerCwdRoot(const MainWindow *main,");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("wantCanonical.isEmpty()"), std::string::npos)
        << "ANTS-1396 INV-4: missing wantCanonical.isEmpty() guard in helper";
    EXPECT_NE(body.find("Source::Unresolvable"), std::string::npos)
        << "ANTS-1396 INV-4: Unresolvable case must be set explicitly "
           "rather than degrading to EmptyFallback / NoMatch";
}

// ANTS-1401 INV-5 — lowest-index tie-break on duplicate tab cwds.
// Walk is `for (int i = 0; i < main->tabCount(); ++i)` — first
// match wins. A reverse-walk regression would silently flip
// `tabIndex` to the highest-index match, changing the
// `caller_cwd_info` envelope (ANTS-1400) under callers.
TEST(TerminalForCallerIsolation, HelperWalksAscendingForLowestIndex) {
    const std::string &rc = rcSource();
    ASSERT_FALSE(rc.empty());
    const std::string body =
        functionBody(rc, "resolveCallerCwdRoot(const MainWindow *main,");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("for (int i = 0;"), std::string::npos)
        << "ANTS-1401 INV-5: helper must walk tabs ascending so the "
           "lowest-index tab wins on a duplicate-cwd tie";
    EXPECT_EQ(body.find("--i"), std::string::npos)
        << "ANTS-1401 INV-5: descending walk would flip the tie-break";
}

// ANTS-1401 — wrapper delegates to the helper. Lock-in that
// `terminalForCaller` does NOT carry its own canonicalisation or
// tab-walk (which would let the wrapper drift from the helper's
// four-case contract). Greps for: a `resolveCallerCwdRoot` call,
// and absence of `canonicalFilePath` (canonicalisation belongs in
// the helper).
TEST(TerminalForCallerIsolation, WrapperDelegatesToHelper) {
    const std::string &mw = mwSource();
    ASSERT_FALSE(mw.empty());
    const std::string body =
        functionBody(mw, "MainWindow::terminalForCaller");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("resolveCallerCwdRoot"), std::string::npos)
        << "ANTS-1401: terminalForCaller must delegate to "
           "ants::resolveCallerCwdRoot rather than re-implement the "
           "four-case decision tree";
    EXPECT_EQ(body.find("canonicalFilePath"), std::string::npos)
        << "ANTS-1401: canonicalisation must live in the helper, "
           "not in terminalForCaller — otherwise the wrapper can "
           "drift from the helper's rules";
}
