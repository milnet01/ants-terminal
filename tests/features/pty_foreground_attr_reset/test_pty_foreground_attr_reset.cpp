// Why this exists: Claude Code emits ESC[2m ... ESC[22m around its dim
// status lines. Kill it between the two and dim stays set -- correct VT
// behaviour, so no parser fix reaches it -- and this host's prompt carries
// no SGR reset to clear it. See spec.md.
//
// The state machine and the ground gate (INV-10) are called directly rather
// than source-grepped: both are pure static functions precisely so they can
// be. INV-9 (the wiring) is the one part that needs the source, because
// proving it behaviourally needs a real pty, a real shell and a real
// external program.

#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>

#include "ptyhandler.h"
#include "vtparser.h"
#include "vtstream.h"
#include "../../_support/srcgrep.h"

#ifndef SRC_PTY_CPP_PATH
#  error "SRC_PTY_CPP_PATH compile definition required"
#endif
#ifndef SRC_VTSTREAM_CPP_PATH
#  error "SRC_VTSTREAM_CPP_PATH compile definition required"
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
    // INV-9 -- ordering. The verdict travels as a signal emitted before the
    // wake's dataReceived, on the same thread, and VtStream feeds the reset.
    const std::string cpp = ants_test::slurpFile(SRC_PTY_CPP_PATH);
    const std::string stream = ants_test::slurpFile(SRC_VTSTREAM_CPP_PATH);

    ASSERT_FALSE(cpp.empty()) << "could not read " << SRC_PTY_CPP_PATH;
    ASSERT_FALSE(stream.empty()) << "could not read " << SRC_VTSTREAM_CPP_PATH;

    EXPECT_TRUE(std::regex_search(
        cpp, std::regex(R"(foregroundReturnedToShell\s*\()")))
        << "INV-9: ptyhandler.cpp never calls foregroundReturnedToShell, so "
           "the state machine is dead code and no reset is ever emitted.";

    EXPECT_NE(cpp.find("tcgetpgrp"), std::string::npos)
        << "INV-9: ptyhandler.cpp does not call tcgetpgrp, so nothing "
           "samples the foreground process group.";

    // ANTS-6658 -- Pty no longer injects the reset as data: only the parser
    // knows whether the exited program's last sequence is complete.
    EXPECT_FALSE(std::regex_search(
        cpp, std::regex(R"(emit\s+dataReceived\s*\([^)]*033\[0m)")))
        << "INV-9: Pty injects the reset into the data stream, where it can "
           "land inside a sequence the parser has half-read (INV-10).";

    const auto emitAt = cpp.find("emit foregroundProgramExited()");
    const auto readAt = cpp.find("::read(m_masterFd, buf");
    EXPECT_NE(emitAt, std::string::npos)
        << "INV-9: ptyhandler.cpp never emits foregroundProgramExited.";
    EXPECT_TRUE(emitAt != std::string::npos && readAt != std::string::npos &&
                emitAt < readAt)
        << "INV-9: the verdict is not emitted before the read loop, so the "
           "reset could reach the parser after the prompt it must precede.";

    EXPECT_TRUE(std::regex_search(
        stream, std::regex(R"(&Pty::foregroundProgramExited\s*,\s*this\s*,\s*&VtStream::onForegroundProgramExited)")))
        << "INV-9: VtStream does not connect Pty::foregroundProgramExited, so "
           "no reset is ever fed.";
}

namespace {

// INV-10 -- a parser whose actions land in `pending`, as VtStream wires it.
struct GateRig {
    VtBatchBuilder pending;
    VtParser parser{[this](const VtAction &a) { pending.add(a); }};
    void feed(const char *bytes) { parser.feed(bytes, static_cast<int>(std::strlen(bytes))); }
    VtBatch take() { VtBatch b; pending.takeInto(b); return b; }
};

bool isSgr(const VtAction &a) {
    return a.type == VtAction::CsiDispatch && a.finalChar == 'm' &&
           a.intermediate.empty();
}

} // namespace

TEST(PtyForegroundAttrReset, ResetFedAtGround) {
    // INV-10 -- between sequences the reset is fed, and recorded in the raw
    // bytes at the point the parser saw it.
    GateRig rig;
    EXPECT_TRUE(VtStream::feedSgrResetIfAtGround(rig.parser, rig.pending))
        << "INV-10: a parser at ground was refused the reset.";
    const VtBatch b = rig.take();
    EXPECT_TRUE(b.rawBytes == QByteArray("\033[0m"))
        << "INV-10: the raw bytes are not exactly the reset.";
    ASSERT_EQ(b.actions.size(), 1u);
    EXPECT_TRUE(isSgr(b.actions[0]) &&
                (b.actions[0].params.empty() || b.actions[0].params == std::vector<int>{0}))
        << "INV-10: the reset did not parse as SGR 0.";
}

TEST(PtyForegroundAttrReset, ResetHeldWhileMidSequence) {
    // INV-10 -- the exited program's last bytes may still be arriving. A
    // reset fed into a half-read sequence would break it, so none is fed
    // until the parser is back at ground.
    struct Case { const char *name; const char *head; const char *tail; };
    const Case cases[] = {
        {"CSI", "\033[3", "1m"},
        {"UTF-8", "\xE2\x82", "\xAC"},
        {"OSC", "\033]0;title", "\a"},
        {"ESC", "\033", "7"},
    };
    for (const Case &c : cases) {
        GateRig rig;
        rig.feed(c.head);
        EXPECT_FALSE(VtStream::feedSgrResetIfAtGround(rig.parser, rig.pending))
            << "INV-10: the reset was fed mid-" << c.name << " sequence.";
        EXPECT_EQ(rig.pending.rawSize(), 0)
            << "INV-10: a held reset still reached the raw bytes (" << c.name << ").";
        rig.feed(c.tail);
        for (const VtAction &a : rig.take().actions)
            EXPECT_FALSE(isSgr(a) && a.params != std::vector<int>{31})
                << "INV-10: an SGR the stream never sent appeared (" << c.name << ").";
        EXPECT_TRUE(VtStream::feedSgrResetIfAtGround(rig.parser, rig.pending))
            << "INV-10: the reset was refused once the " << c.name
            << " sequence completed.";
    }
}
