// Why this exists: Pty::m_childPid is written on the parse-worker thread
// (onReadReady's reap branch) and read on the GUI thread through
// TerminalWidget::ptyChildPid. See spec.md.
//
// Source-grep, and the spec says why: a test that races the two threads
// is non-deterministic by construction and a green run would prove
// nothing. The declaration is the fix.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_PTY_H_PATH
#  error "SRC_PTY_H_PATH compile definition required"
#endif

TEST(PtyChildPidAtomic, Main) {
    const std::string hdr = ants_test::slurpFile(SRC_PTY_H_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1 — the member is atomic.
    const std::regex atomicMember(
        R"(std::atomic\s*<\s*pid_t\s*>\s*m_childPid)");
    if (!std::regex_search(hdr, atomicMember)) {
        fail("INV-1: m_childPid is not declared std::atomic<pid_t>. It is "
             "written on the parse-worker thread when the child is reaped "
             "at EOF, and read on the GUI thread by ptyChildPid — a data "
             "race, whose visible effect is /proc lookups against a PID "
             "the kernel may have recycled.");
    }

    // INV-2 — no plain declaration left behind. The alternation on \n is
    // load-bearing: std::regex defaults to ECMAScript without the multiline
    // flag, so a bare ^ anchors to the start of the WHOLE string and this
    // check passed against the very declaration it exists to forbid.
    const std::regex plainMember(R"((?:^|\n)\s*pid_t\s+m_childPid\b)");
    if (std::regex_search(hdr, plainMember)) {
        fail("INV-2: a plain `pid_t m_childPid` declaration is still "
             "present in src/ptyhandler.h.");
    }

    // INV-3 — the type's header is included, not inherited transitively.
    if (hdr.find("#include <atomic>") == std::string::npos) {
        fail("INV-3: src/ptyhandler.h does not include <atomic>. Relying "
             "on a transitive include for a type used in a public header "
             "is how INV-1 breaks on another toolchain.");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/pty_childpid_atomic/spec.md for context\n",
            failures);
    }
    ASSERT_EQ(0, failures);
}
