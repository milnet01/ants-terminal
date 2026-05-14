// Feature-conformance test for spec.md — asserts an open-but-uncommitted
// OSC 8 hyperlink is RESET (closed) by a resize, not clamped to a
// stale row. Pre-indie-review-2026-05-14 lane-1 H2, the resize clamped
// m_hyperlinkStartRow/Col to the new bounds, which (a) didn't match
// the comment claiming "close the active hyperlink on any resize"
// and (b) reintroduced the stale-coordinate bug — a shrink pulled
// startRow into a row the text was never on.
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>
#include <cstdio>
#include <gtest/gtest.h>
#include <string>

namespace {

struct Harness {
    TerminalGrid grid{24, 80};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

std::string cup(int row, int col) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
    return buf;
}

// Scan every row of the grid's hyperlink table for any span whose URI
// contains `needle`. Returns the row it was found on, or -1 if absent.
int findUriRow(const TerminalGrid &g, int rows, const std::string &needle) {
    for (int r = 0; r < rows; ++r) {
        const auto &spans = g.screenHyperlinks(r);
        for (const auto &s : spans) {
            if (s.uri.find(needle) != std::string::npos) return r;
        }
    }
    return -1;
}

}  // namespace

TEST(HyperlinkResizeClamp, Main) {
    std::setlocale(LC_CTYPE, "");

    // I1/I2 — Scenario: open hyperlink at (20, 50), shrink grid to
    // (5, 10), then close hyperlink. Post-fix: the resize closes the
    // in-progress hyperlink, so the subsequent close sequence is a
    // no-op and no span lands in the new grid for that URL.
    {
        Harness h;
        h.feed(cup(20, 50));
        h.feed("\033]8;;https://reset.example/\x07");
        h.feed("text");
        h.grid.resize(5, 10);
        h.feed("\033]8;;\x07");

        const int row = findUriRow(h.grid, 5, "reset.example");
        EXPECT_EQ(row, -1)
            << "I1/I2: resize did not close the open OSC 8 hyperlink "
               "(stale span attached to row " << row << ")";
    }

    // I3 — A fresh OSC 8 sequence after the resize works normally.
    // Build a 24x80 grid, shrink to 10x40 with an open hyperlink, then
    // emit a complete OSC 8 open+text+close on row 2 of the new grid.
    {
        Harness h;
        h.feed(cup(20, 50));
        h.feed("\033]8;;https://orphan.example/\x07");
        h.feed("garbage");
        h.grid.resize(10, 40);                 // closes the orphan
        h.feed(cup(2, 0));                     // row 2 (0-indexed)
        h.feed("\033]8;;https://fresh.example/\x07");
        h.feed("ok");
        h.feed("\033]8;;\x07");

        const int orphanRow = findUriRow(h.grid, 10, "orphan.example");
        EXPECT_EQ(orphanRow, -1)
            << "I1: pre-resize orphan hyperlink leaked into post-resize grid "
               "(found on row " << orphanRow << ")";

        const int freshRow = findUriRow(h.grid, 10, "fresh.example");
        EXPECT_EQ(freshRow, 2)
            << "I3: fresh OSC 8 sequence after resize did not commit "
               "on its expected row (got " << freshRow << ", want 2)";
    }

    std::printf("hyperlink_resize_clamp: resize closes the open span; fresh OSC 8 works\n");
}
