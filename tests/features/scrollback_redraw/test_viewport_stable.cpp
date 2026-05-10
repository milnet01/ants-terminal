// Feature-conformance test for spec.md §Viewport-stable contract.
//
// Asserts that when a user has scrolled up in history (m_scrollOffset > 0
// in TerminalWidget) and new content streams in, the logical content the
// user sees at any given viewport row does not change across the stream.
//
// The bug this test catches (user report, 2026-04-15):
//   User scrolls up a few lines so their viewport includes the tail of
//   scrollback plus the head of the screen. Claude Code streams output.
//   Because the 0.6.21 pause and/or the 0.6.22/0.6.24 CSI-clear
//   suppression window skip scrollback pushes while their window is
//   armed, the anchor logic in TerminalWidget::onOutputReceived doesn't
//   fire — but the *screen* still scrolls on every LF, so the bottom
//   rows of the user's viewport (the portion that overlaps the screen)
//   are overwritten in place. From the user's perspective, "line 251
//   becomes line 250" — the view shifts up even though they're pinned.
//
// Because this test links against only src/terminalgrid.cpp and
// src/vtparser.cpp (no Qt widget), we simulate the widget's scroll-offset
// + anchor state in the test directly. The anchor reproduction matches
// TerminalWidget::onOutputReceived lines 1708-1727 exactly:
//
//   pushedBefore = grid.scrollbackPushed();
//   feed(...);
//   if (scrollOffset > 0 && grid.scrollbackPushed() > pushedBefore) {
//       int added = grid.scrollbackPushed() - pushedBefore;
//       scrollOffset = min(scrollOffset + added, grid.scrollbackSize());
//   }
//
// ANTS-1217 Phase 2: migrated to GoogleTest TEST() blocks under Suite
// ScrollbackRedraw (sibling to test_redraw.cpp).

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

// Read the cell at the user's viewport row `vr`. Reproduces
// TerminalWidget::cellAtGlobal(globalLine, col) for globalLine =
// viewStart + vr.
uint32_t cellAtViewport(const TerminalGrid &grid, int scrollOffset,
                        int vr, int col) {
    const int sbSize = grid.scrollbackSize();
    const int viewStart = sbSize - scrollOffset;
    const int globalLine = viewStart + vr;
    if (globalLine >= 0 && globalLine < sbSize) {
        const auto &line = grid.scrollbackLine(globalLine);
        if (col >= 0 && col < static_cast<int>(line.size()))
            return line[col].codepoint;
    } else {
        const int sr = globalLine - sbSize;
        if (sr >= 0 && sr < grid.rows() && col >= 0 && col < grid.cols())
            return grid.cellAt(sr, col).codepoint;
    }
    return ' ';
}

// Snapshot the full viewport row by concatenating printable cells.
std::string snapshotRow(const TerminalGrid &grid, int scrollOffset, int vr) {
    std::string out;
    out.reserve(kCols);
    for (int c = 0; c < kCols; ++c) {
        uint32_t cp = cellAtViewport(grid, scrollOffset, vr, c);
        out.push_back((cp >= 0x20 && cp < 0x7F) ? static_cast<char>(cp) : ' ');
    }
    // Trim trailing spaces for readable diff output.
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

struct Snapshot {
    std::vector<std::string> rows;
};

Snapshot snapshotViewport(const TerminalGrid &grid, int scrollOffset) {
    Snapshot s;
    s.rows.reserve(kRows);
    for (int r = 0; r < kRows; ++r)
        s.rows.push_back(snapshotRow(grid, scrollOffset, r));
    return s;
}

// Reproduce TerminalWidget's onOutputReceived lifecycle: set the pause
// flag, feed, then advance scrollOffset by any pushes that happened.
// Returns the new scrollOffset.
int feedWithAnchor(TerminalGrid &grid, VtParser &parser,
                   int scrollOffset, const std::string &data) {
    grid.setScrollbackInsertPaused(scrollOffset > 0);
    const uint64_t pushedBefore = grid.scrollbackPushed();
    parser.feed(data.data(), static_cast<int>(data.size()));
    if (scrollOffset > 0) {
        const uint64_t added64 = grid.scrollbackPushed() - pushedBefore;
        if (added64 > 0) {
            const int sbSize = grid.scrollbackSize();
            const int added = (added64 > static_cast<uint64_t>(sbSize))
                                ? sbSize : static_cast<int>(added64);
            scrollOffset = std::min(scrollOffset + added, sbSize);
        }
    }
    return scrollOffset;
}

int runScenario(const char *scenarioName,
                int userScrollOffset,
                const char *preStreamClearSeq) {
    TerminalGrid grid(kRows, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });

    int scrollOffset = 0;

    // Phase 1 — fill the terminal with enough content that scrollback
    // has at least as many lines as the user wants to scroll up by, plus
    // the full screen. Each "pre-###" line is unique so we can detect
    // content shifts exactly. 200 lines => scrollback of ~176 + 24 on
    // screen, enough slack for the large-offset scenarios below.
    constexpr int kFillLines = 200;
    for (int i = 0; i < kFillLines; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "pre-%03d\r\n", i);
        scrollOffset = feedWithAnchor(grid, parser, scrollOffset, buf);
    }

    // Phase 2 — user scrolls up by the requested amount. This is a
    // viewport-only change; no grid interaction needed beyond setting
    // our local scrollOffset. The widget will flip setScrollbackInsertPaused
    // on the next feed.
    scrollOffset = userScrollOffset;

    // Phase 3 — snapshot the viewport as the user sees it RIGHT NOW.
    const Snapshot before = snapshotViewport(grid, scrollOffset);

    // Phase 4 — optionally emit the pre-stream clear sequence. If non-empty,
    // this arms the CSI-clear suppression window in the grid, reproducing
    // the Claude Code "CSI H; CSI 0J; <content>" streaming idiom that
    // triggered the user report.
    if (preStreamClearSeq && *preStreamClearSeq) {
        scrollOffset = feedWithAnchor(grid, parser, scrollOffset,
                                      preStreamClearSeq);
    }

    // Phase 5 — stream new content. Enough lines to trigger several
    // scrollUps, which is what causes the viewport to shift in the
    // broken implementation.
    constexpr int kStreamLines = 30;  // > kRows so every viewport row
                                      // could plausibly be disturbed.
    for (int i = 0; i < kStreamLines; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "post-%03d\r\n", i);
        scrollOffset = feedWithAnchor(grid, parser, scrollOffset, buf);
    }

    // Phase 6 — snapshot again at the (possibly-updated) scrollOffset.
    const Snapshot after = snapshotViewport(grid, scrollOffset);

    // Invariant: every viewport row shows the same content before and after.
    int mismatches = 0;
    for (int r = 0; r < kRows; ++r) {
        if (before.rows[r] != after.rows[r]) {
            if (mismatches < 4) {
                std::fprintf(stderr,
                             "  row %2d mismatch:\n"
                             "    before: \"%s\"\n"
                             "    after:  \"%s\"\n",
                             r, before.rows[r].c_str(), after.rows[r].c_str());
            }
            ++mismatches;
        }
    }

    std::fprintf(stderr,
                 "[%s] scrollOffset=%d  mismatches=%d/%d  %s\n",
                 scenarioName, userScrollOffset, mismatches, kRows,
                 (mismatches == 0 ? "PASS" : "FAIL"));

    return mismatches == 0 ? 0 : 1;
}

}  // namespace

// Scenario matrix: scrollOffset varies from "barely scrolled up" to "scrolled
// far into history." Without a pre-stream clear, the 0.6.21 pause fires:
// paused=true, pushes skipped, no anchor advance. Before the 0.6.25 fix, ALL
// offsets failed; after the fix, all must pass.
TEST(ScrollbackRedraw, ViewportStablePauseOnlyOffset1) {
    EXPECT_EQ(0, runScenario("pause-only/offset-1", 1, ""));
}
TEST(ScrollbackRedraw, ViewportStablePauseOnlyOffset12) {
    EXPECT_EQ(0, runScenario("pause-only/offset-12", 12, ""));
}
TEST(ScrollbackRedraw, ViewportStablePauseOnlyOffset50) {
    EXPECT_EQ(0, runScenario("pause-only/offset-50", 50, ""));
}
TEST(ScrollbackRedraw, ViewportStablePauseOnlyOffset120) {
    EXPECT_EQ(0, runScenario("pause-only/offset-120", 120, ""));
}

// With a pre-stream clear, the 0.6.22/0.6.24 suppression window arms too.
// Deep-scroll users (viewport entirely in scrollback) must stay stable;
// shallow scrolls are inherently unstable because the clear wipes on-screen
// content the user was reading — only deep-scroll cases asserted here.
TEST(ScrollbackRedraw, ViewportStableCsi0jClearOffset50) {
    EXPECT_EQ(0, runScenario("csi-0J-clear/offset-50", 50, "\x1b[H\x1b[0J"));
}
TEST(ScrollbackRedraw, ViewportStableCsi0jClearOffset120) {
    EXPECT_EQ(0, runScenario("csi-0J-clear/offset-120", 120, "\x1b[H\x1b[0J"));
}

TEST(ScrollbackRedraw, ViewportStableControlOffset24) {
    EXPECT_EQ(0, runScenario("control/offset-24-fully-in-scrollback", 24, ""));
}
