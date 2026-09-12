// Feature-conformance test for spec.md — asserts TerminalGrid's content
// revision is stable while idle and moves on every mutation path the
// session blob records. ANTS-5030.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

struct Probe {
    TerminalGrid grid;
    VtParser parser;

    Probe()
        : grid(kRows, kCols),
          parser([this](const VtAction &a) { grid.processAction(a); }) {}

    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

}  // namespace

// INV-1 — idle is idle. Reading the grid must not move the counter, or
// every tab looks changed and the skip never fires.
TEST(GridContentRevision, IdleGridDoesNotChange) {
    Probe probe;
    probe.feed("hello");
    const uint64_t rev = probe.grid.contentRevision();

    // Read-only traffic of the kind a paint pass and a save both perform.
    for (int row = 0; row < probe.grid.rows(); ++row)
        for (int col = 0; col < probe.grid.cols(); ++col)
            (void)probe.grid.cellAt(row, col);
    (void)probe.grid.scrollbackSize();
    (void)probe.grid.windowTitle();
    (void)probe.grid.cursorRow();

    EXPECT_EQ(probe.grid.contentRevision(), rev)
        << "reading the grid moved the content revision";
}

// INV-2 — printing changes it.
TEST(GridContentRevision, PrintingChangesIt) {
    Probe probe;
    const uint64_t rev = probe.grid.contentRevision();
    probe.feed("X");
    EXPECT_NE(probe.grid.contentRevision(), rev)
        << "a printed character did not move the content revision";
}

// INV-3 — a scrollback push changes it. Enough newlines to push the
// first screen row off the top.
TEST(GridContentRevision, ScrollbackPushChangesIt) {
    Probe probe;
    probe.feed("seed\r\n");
    const int before = probe.grid.scrollbackSize();
    const uint64_t rev = probe.grid.contentRevision();
    for (int i = 0; i < kRows + 2; ++i) probe.feed("line\r\n");
    ASSERT_GT(probe.grid.scrollbackSize(), before)
        << "test did not actually push a scrollback line";
    EXPECT_NE(probe.grid.contentRevision(), rev)
        << "a scrollback push did not move the content revision";
}

// INV-4 — resize changes it; the blob records the dimensions.
TEST(GridContentRevision, ResizeChangesIt) {
    Probe probe;
    const uint64_t rev = probe.grid.contentRevision();
    probe.grid.resize(kRows + 4, kCols + 4);
    EXPECT_NE(probe.grid.contentRevision(), rev)
        << "a resize did not move the content revision";
}

// INV-5 — the window title is in the blob, so setting it must move it.
TEST(GridContentRevision, TitleChangeChangesIt) {
    Probe probe;
    const uint64_t rev = probe.grid.contentRevision();
    probe.feed("\x1b]0;a new title\x07");
    ASSERT_EQ(probe.grid.windowTitle(), QStringLiteral("a new title"))
        << "test did not actually set the title";
    EXPECT_NE(probe.grid.contentRevision(), rev)
        << "a title change did not move the content revision";
}

// INV-6 — the cursor position is in the blob.
TEST(GridContentRevision, CursorMoveChangesIt) {
    Probe probe;
    const uint64_t rev = probe.grid.contentRevision();
    probe.feed("\x1b[10;20H");
    ASSERT_NE(probe.grid.cursorRow(), 0) << "test did not actually move the cursor";
    EXPECT_NE(probe.grid.contentRevision(), rev)
        << "a cursor move did not move the content revision";
}

// INV-7 — clearing the screen changes it.
TEST(GridContentRevision, ClearScreenChangesIt) {
    Probe probe;
    probe.feed("content");
    const uint64_t rev = probe.grid.contentRevision();
    probe.feed("\x1b[2J");
    EXPECT_NE(probe.grid.contentRevision(), rev)
        << "clearing the screen did not move the content revision";
}
