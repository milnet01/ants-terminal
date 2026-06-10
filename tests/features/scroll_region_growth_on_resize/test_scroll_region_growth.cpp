// Feature-conformance test for spec.md (ANTS-1194) — locks the
// "default scroll region grows / shrinks with the grid on resize"
// contract. Pre-fix code only clamped, which silently broke window-
// grow when no DECSTBM was ever set: tab opened at 24 rows, grew to
// 60, scrollBottom stayed at 23, output piled into top 24 rows.
//
// Direct assertions via TerminalGrid::scrollTop() / scrollBottom().
// No GUI, no PTY, no parser dependency for the resize-only tests.
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <string>

namespace {

// Canonical default grid geometry — 24×80 is the size a tab opens at
// (ANTS-1598). The resize() targets below stay raw literals on purpose:
// they're the varied parameters under test (grow to 60, shrink to 30,
// grow to 80×100), not "the default size".
constexpr int kRows = 24;
constexpr int kCols = 80;

#undef EXPECT
#define EXPECT(cond, ...) do {                                  \
    if (!(cond)) {                                              \
        char _ants_msg[512];                                    \
        std::snprintf(_ants_msg, sizeof(_ants_msg), __VA_ARGS__); \
        ADD_FAILURE_AT(__FILE__, __LINE__) << _ants_msg;        \
    }                                                           \
} while (0)

// 1-indexed CSI args; emit DECSTBM "CSI top;bottom r" via parser
// so the grid records the region the same way it would from a real
// PTY stream.
void setRegion(VtParser &parser, int topRow0, int bottomRow0) {
    std::string s = "\x1b[" + std::to_string(topRow0 + 1) + ";" +
                    std::to_string(bottomRow0 + 1) + "r";
    parser.feed(s.data(), static_cast<int>(s.size()));
}

void enterAltScreen(VtParser &parser) {
    std::string s = "\x1b[?1049h";
    parser.feed(s.data(), static_cast<int>(s.size()));
}

}  // namespace

// ----- INV-1 — implicit (full-screen) primary region grows on resize.
TEST(ScrollRegionGrowthOnResize, DefaultPrimaryGrows) {
    TerminalGrid grid(kRows, kCols);
    EXPECT(grid.scrollTop() == 0 && grid.scrollBottom() == 23,
           "INV-1 setup: scrollTop=%d scrollBottom=%d, expected 0/23",
           grid.scrollTop(), grid.scrollBottom());
    grid.resize(60, 80);
    EXPECT(grid.scrollTop() == 0,
           "INV-1: scrollTop=%d after grow, expected 0", grid.scrollTop());
    EXPECT(grid.scrollBottom() == 59,
           "INV-1: scrollBottom=%d after grow to 60 rows, expected 59 "
           "(this was the user-reported bug — bottom stayed at 23, "
           "output piled into top 24 rows)", grid.scrollBottom());
}

// ----- INV-2 — implicit (full-screen) primary region shrinks on resize.
TEST(ScrollRegionGrowthOnResize, DefaultPrimaryShrinks) {
    TerminalGrid grid(60, kCols);
    grid.resize(24, 80);
    EXPECT(grid.scrollTop() == 0,
           "INV-2: scrollTop=%d after shrink, expected 0", grid.scrollTop());
    EXPECT(grid.scrollBottom() == 23,
           "INV-2: scrollBottom=%d after shrink to 24, expected 23",
           grid.scrollBottom());
}

// ----- INV-3 — explicit DECSTBM preserved on grow (NOT auto-widened).
TEST(ScrollRegionGrowthOnResize, ExplicitPreservedOnGrow) {
    TerminalGrid grid(kRows, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    setRegion(parser,4, 14);  // DECSTBM 5;15
    EXPECT(grid.scrollTop() == 4 && grid.scrollBottom() == 14,
           "INV-3 setup: scrollTop=%d scrollBottom=%d, expected 4/14",
           grid.scrollTop(), grid.scrollBottom());
    grid.resize(60, 80);
    EXPECT(grid.scrollTop() == 4,
           "INV-3: scrollTop=%d after grow with explicit DECSTBM, "
           "expected 4 (preserved)", grid.scrollTop());
    EXPECT(grid.scrollBottom() == 14,
           "INV-3: scrollBottom=%d after grow with explicit DECSTBM, "
           "expected 14 (preserved, NOT auto-widened to 59)",
           grid.scrollBottom());
}

// ----- INV-4 — explicit DECSTBM clamped on shrink below bottom.
TEST(ScrollRegionGrowthOnResize, ExplicitClampedOnShrinkBelowBottom) {
    TerminalGrid grid(60, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    setRegion(parser,4, 54);  // DECSTBM 5;55
    EXPECT(grid.scrollTop() == 4 && grid.scrollBottom() == 54,
           "INV-4 setup: scrollTop=%d scrollBottom=%d, expected 4/54",
           grid.scrollTop(), grid.scrollBottom());
    grid.resize(30, 80);
    EXPECT(grid.scrollTop() == 4,
           "INV-4: scrollTop=%d after shrink, expected 4 (preserved)",
           grid.scrollTop());
    EXPECT(grid.scrollBottom() == 29,
           "INV-4: scrollBottom=%d after shrink to 30 rows, expected 29 "
           "(clamped to new max — ANTS-1130 contract)",
           grid.scrollBottom());
}

// ----- INV-5 — alt scroll-region grows on resize when full-screen.
//
// Inside alt mode, m_scrollTop/m_scrollBottom track the alt region
// (defaults to full-screen). Resize must widen those, same as primary.
TEST(ScrollRegionGrowthOnResize, AltDefaultGrows) {
    TerminalGrid grid(kRows, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    enterAltScreen(parser);
    EXPECT(grid.altScreenActive(),
           "INV-5 setup: alt-screen not active");
    EXPECT(grid.scrollTop() == 0 && grid.scrollBottom() == 23,
           "INV-5 setup: scrollTop=%d scrollBottom=%d, expected 0/23",
           grid.scrollTop(), grid.scrollBottom());
    grid.resize(60, 80);
    EXPECT(grid.scrollBottom() == 59,
           "INV-5: scrollBottom=%d after grow in alt mode, expected 59",
           grid.scrollBottom());
}

// ----- INV-6 — alt explicit DECSTBM preserved on grow.
TEST(ScrollRegionGrowthOnResize, AltExplicitPreservedOnGrow) {
    TerminalGrid grid(kRows, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    enterAltScreen(parser);
    setRegion(parser,4, 14);
    grid.resize(60, 80);
    EXPECT(grid.scrollTop() == 4 && grid.scrollBottom() == 14,
           "INV-6: alt scrollTop=%d scrollBottom=%d after grow, "
           "expected 4/14 (preserved)",
           grid.scrollTop(), grid.scrollBottom());
}

// ----- INV-7 — sequential grow / shrink stays consistent.
TEST(ScrollRegionGrowthOnResize, GrowShrinkSequence) {
    TerminalGrid grid(kRows, kCols);
    grid.resize(60, 80);
    EXPECT(grid.scrollBottom() == 59,
           "INV-7 step 1 (grow 24→60): scrollBottom=%d, expected 59",
           grid.scrollBottom());
    grid.resize(30, 80);
    EXPECT(grid.scrollBottom() == 29,
           "INV-7 step 2 (shrink 60→30): scrollBottom=%d, expected 29",
           grid.scrollBottom());
    grid.resize(24, 80);
    EXPECT(grid.scrollBottom() == 23,
           "INV-7 step 3 (shrink 30→24): scrollBottom=%d, expected 23",
           grid.scrollBottom());
    grid.resize(80, 100);
    EXPECT(grid.scrollBottom() == 79,
           "INV-7 step 4 (grow 24→80): scrollBottom=%d, expected 79",
           grid.scrollBottom());
}


