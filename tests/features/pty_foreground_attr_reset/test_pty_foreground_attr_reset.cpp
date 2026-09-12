// Why this exists: Claude Code emits ESC[2m ... ESC[22m around its dim
// status lines. Kill it between the two and dim stays set -- correct VT
// behaviour, so no parser fix reaches it -- and this host's prompt carries
// no SGR reset to clear it. See spec.md.
//
// The state machine is called directly rather than source-grepped: it is a
// pure static function precisely so it can be. INV-9 (the wiring) is the
// one part that needs the source, because proving it behaviourally needs a
// real pty, a real shell and a real external program.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>

#include "ptyhandler.h"
#include "../../_support/srcgrep.h"

#ifndef SRC_PTY_CPP_PATH
#  error "SRC_PTY_CPP_PATH compile definition required"
#endif

namespace {

constexpr pid_t kShell = 4242;   // the forkpty child's pgid
constexpr pid_t kProgA = 5001;   // a foreground program
constexpr pid_t kProgB = 5002;   // a different foreground program

// Feed one sample, carrying `lastSeen` across calls like the real caller.
bool sample(pid_t fg, pid_t &lastSeen, pid_t shell = kShell) {
    return Pty::foregroundReturnedToShell(fg, shell, lastSeen);
}

} // namespace

TEST(PtyForegroundAttrReset, FirstSampleNeverFires) {
    // INV-1 -- a terminal opening with the shell already idle must not
    // reset on its very first read.
    pid_t last = -1;
    EXPECT_FALSE(sample(kShell, last))
        << "INV-1: the first sample fired. lastSeen starts at -1 and must "
           "be treated as 'unknown', not as 'was not the shell'.";
    EXPECT_EQ(last, kShell) << "INV-1: the first sample was not recorded.";
}

TEST(PtyForegroundAttrReset, IdleShellIsSilent) {
    // INV-2 -- steady state. If this fired, every read would reset.
    pid_t last = -1;
    sample(kShell, last);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(sample(kShell, last))
            << "INV-2: an idle shell fired on read " << i
            << ". Steady state must be silent.";
    }
}

TEST(PtyForegroundAttrReset, ProgramStartingIsSilent) {
    // INV-3 -- shell -> program is a launch, not an exit.
    pid_t last = -1;
    sample(kShell, last);
    EXPECT_FALSE(sample(kProgA, last))
        << "INV-3: a program taking the foreground fired.";
}

TEST(PtyForegroundAttrReset, ProgramProducingOutputIsSilent) {
    // INV-4 -- the dangerous false positive: resetting mid-output would
    // strip the styling of the program currently drawing.
    pid_t last = -1;
    sample(kShell, last);
    sample(kProgA, last);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(sample(kProgA, last))
            << "INV-4: fired while the program was still producing output "
               "(read " << i << "). This would strip a running TUI's own "
               "styling.";
    }
}

TEST(PtyForegroundAttrReset, ProgramToProgramIsSilent) {
    // INV-5 -- only a return to the SHELL counts.
    pid_t last = -1;
    sample(kShell, last);
    sample(kProgA, last);
    EXPECT_FALSE(sample(kProgB, last))
        << "INV-5: fired on a change between two non-shell groups.";
}

TEST(PtyForegroundAttrReset, ProgramExitFiresExactlyOnce) {
    // INV-6 -- the whole point, and it must not repeat.
    pid_t last = -1;
    sample(kShell, last);
    sample(kProgA, last);
    EXPECT_TRUE(sample(kShell, last))
        << "INV-6: a foreground program exited and the reset did not fire. "
           "This is the reported defect: the prompt written next inherits "
           "the program's dim attribute.";
    EXPECT_FALSE(sample(kShell, last))
        << "INV-6: fired a second time on an unchanged sample.";
}

TEST(PtyForegroundAttrReset, UnreadableForegroundNeverFires) {
    // INV-7 -- may miss, must never invent.
    pid_t last = -1;
    sample(kShell, last);
    sample(kProgA, last);

    EXPECT_FALSE(sample(-1, last))
        << "INV-7: an unreadable foreground (-1) fired.";
    EXPECT_EQ(last, -1)
        << "INV-7: the failed sample was not recorded, so the next sample "
           "could read as a transition out of a stale value.";
    EXPECT_FALSE(sample(kShell, last))
        << "INV-7: fired straight after an unreadable sample. The bias must "
           "be to miss a transition rather than invent one.";

    pid_t zero = -1;
    sample(kShell, zero);
    sample(kProgA, zero);
    EXPECT_FALSE(sample(0, zero)) << "INV-7: a zero foreground fired.";
}

TEST(PtyForegroundAttrReset, ReapedChildNeverFires) {
    // INV-8 -- no shell to return to.
    pid_t last = -1;
    sample(kProgA, last, -1);
    EXPECT_FALSE(sample(kShell, last, -1))
        << "INV-8: fired with shellPgid <= 0, i.e. the child already reaped.";
    pid_t zero = -1;
    sample(kProgA, zero, 0);
    EXPECT_FALSE(sample(kShell, zero, 0))
        << "INV-8: fired with shellPgid == 0.";
}

TEST(PtyForegroundAttrReset, WiredIntoTheReadPath) {
    // INV-9 -- ordering. A separate signal could overtake queued parse
    // batches; only the data stream preserves "reset before the prompt".
    const std::string cpp = ants_test::slurpFile(SRC_PTY_CPP_PATH);

    ASSERT_FALSE(cpp.empty()) << "could not read " << SRC_PTY_CPP_PATH;

    EXPECT_TRUE(std::regex_search(
        cpp, std::regex(R"(foregroundReturnedToShell\s*\()")))
        << "INV-9: ptyhandler.cpp never calls foregroundReturnedToShell, so "
           "the state machine is dead code and no reset is ever emitted.";

    EXPECT_NE(cpp.find("tcgetpgrp"), std::string::npos)
        << "INV-9: ptyhandler.cpp does not call tcgetpgrp, so nothing "
           "samples the foreground process group.";

    // The reset must be emitted as DATA, not as a bespoke signal.
    EXPECT_TRUE(std::regex_search(
        cpp, std::regex(R"(emit\s+dataReceived\s*\([^)]*033\[0m)")))
        << "INV-9: the reset is not emitted into the data stream. It must "
           "travel as bytes so it cannot overtake the parse batches it has "
           "to precede.";
}
