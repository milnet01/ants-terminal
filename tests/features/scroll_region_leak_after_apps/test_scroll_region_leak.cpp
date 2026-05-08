// Feature-conformance test for spec.md (ANTS-1187) — locks the
// observable behaviour around DECSTBM scroll-region reset paths and the
// alt-screen save/restore semantics that should not leak alt's scroll
// region back onto main.
//
// User-facing symptom: long-running server tab renders new output only
// in the upper half of the terminal; bottom half stays blank. Root
// cause hypothesis is a stale DECSTBM left behind by a prior process
// (claude-code footer, ncurses progress bar, alt-screen TUI exit
// without restore). This test pins the standard reset paths and the
// alt-screen save/restore so future refactors can't silently break
// any of them.
//
// Direct assertions via TerminalGrid::scrollTop() / scrollBottom() /
// altScreenActive() (added in the same change as this test).
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <string>

namespace {

constexpr int kRows = 20;
constexpr int kCols = 16;

int g_failures = 0;

#define EXPECT(cond, ...) do {                                  \
    if (!(cond)) {                                              \
        std::fprintf(stderr, "FAIL [%s:%d] ", __FILE__, __LINE__); \
        std::fprintf(stderr, __VA_ARGS__);                      \
        std::fprintf(stderr, "\n");                             \
        ++g_failures;                                           \
    }                                                           \
} while (0)

struct Harness {
    TerminalGrid grid;
    VtParser parser;
    Harness() : grid(kRows, kCols),
                parser([this](const VtAction &a) { grid.processAction(a); }) {}
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
    // 1-indexed CSI args; emit DECSTBM "CSI top;bottom r"
    void setRegion(int topRow0, int bottomRow0) {
        feed("\x1b[" + std::to_string(topRow0 + 1) + ";" +
             std::to_string(bottomRow0 + 1) + "r");
    }
    void resetRegion()    { feed("\x1b[r"); }       // CSI r — full screen
    void ris()            { feed("\x1b" "c"); }     // ESC c — RIS (split: \x consumes hex digits greedily)
    void enterAltScreen() { feed("\x1b[?1049h"); }
    void exitAltScreen()  { feed("\x1b[?1049l"); }
};

} // namespace

// ----- INV-1 — DECSTBM is recorded by the grid.
// Direct: setRegion(5, 15) → scrollTop()==5, scrollBottom()==15.
static void testInv1_DecstbmRecorded() {
    Harness h;
    h.setRegion(5, 15);
    EXPECT(h.grid.scrollTop() == 5,
           "INV-1: scrollTop=%d, expected 5", h.grid.scrollTop());
    EXPECT(h.grid.scrollBottom() == 15,
           "INV-1: scrollBottom=%d, expected 15", h.grid.scrollBottom());
}

// ----- INV-2 — `CSI r` (no params) resets to full screen.
static void testInv2_CsiRReset() {
    Harness h;
    h.setRegion(5, 15);
    h.resetRegion();
    EXPECT(h.grid.scrollTop() == 0,
           "INV-2: scrollTop=%d after CSI r, expected 0",
           h.grid.scrollTop());
    EXPECT(h.grid.scrollBottom() == kRows - 1,
           "INV-2: scrollBottom=%d after CSI r, expected %d",
           h.grid.scrollBottom(), kRows - 1);
}

// ----- INV-3 — RIS (`ESC c`) resets to full screen.
static void testInv3_RisReset() {
    Harness h;
    h.setRegion(5, 15);
    h.ris();
    EXPECT(h.grid.scrollTop() == 0,
           "INV-3: scrollTop=%d after RIS, expected 0",
           h.grid.scrollTop());
    EXPECT(h.grid.scrollBottom() == kRows - 1,
           "INV-3: scrollBottom=%d after RIS, expected %d",
           h.grid.scrollBottom(), kRows - 1);
}

// ----- INV-4 — Alt-screen DECSTBM doesn't leak back to main on exit.
//
// Start from full-screen main region. Enter alt-screen. Set DECSTBM
// in alt to [0, 4]. Exit alt-screen. Main scroll region must be the
// pre-entry full-screen [0, kRows-1], NOT the alt's [0, 4].
static void testInv4_AltDecstbmDoesNotLeak() {
    Harness h;
    EXPECT(h.grid.scrollTop() == 0 && h.grid.scrollBottom() == kRows - 1,
           "INV-4 setup: pre-entry main region not full-screen");

    h.enterAltScreen();
    EXPECT(h.grid.altScreenActive(),
           "INV-4 setup: alt-screen did not activate");
    h.setRegion(0, 4);  // narrow alt region
    EXPECT(h.grid.scrollTop() == 0 && h.grid.scrollBottom() == 4,
           "INV-4 setup: alt-screen DECSTBM [%d, %d], expected [0, 4]",
           h.grid.scrollTop(), h.grid.scrollBottom());

    h.exitAltScreen();
    EXPECT(!h.grid.altScreenActive(),
           "INV-4 setup: alt-screen did not deactivate");
    EXPECT(h.grid.scrollTop() == 0,
           "INV-4: scrollTop=%d after alt-exit (alt's narrowed region "
           "leaked), expected 0",
           h.grid.scrollTop());
    EXPECT(h.grid.scrollBottom() == kRows - 1,
           "INV-4: scrollBottom=%d after alt-exit (alt's narrowed "
           "region leaked), expected %d",
           h.grid.scrollBottom(), kRows - 1);
}

// ----- INV-5 — Pre-alt-entry main DECSTBM survives an alt session.
//
// Set main region to [3, 12] BEFORE alt entry. Enter alt, set alt
// region to [0, 4], exit alt. Main must be back to [3, 12] — its
// pre-entry value, not full-screen and not alt's [0, 4].
static void testInv5_MainPreEntryRegionPreserved() {
    Harness h;
    h.setRegion(3, 12);
    EXPECT(h.grid.scrollTop() == 3 && h.grid.scrollBottom() == 12,
           "INV-5 setup: pre-entry main region [%d, %d], expected [3, 12]",
           h.grid.scrollTop(), h.grid.scrollBottom());

    h.enterAltScreen();
    h.setRegion(0, 4);   // narrow alt region
    h.exitAltScreen();

    EXPECT(h.grid.scrollTop() == 3,
           "INV-5: scrollTop=%d after alt-exit, expected 3 (pre-entry)",
           h.grid.scrollTop());
    EXPECT(h.grid.scrollBottom() == 12,
           "INV-5: scrollBottom=%d after alt-exit, expected 12 "
           "(pre-entry)",
           h.grid.scrollBottom());
}

// ----- INV-6 — User-facing print-reaches-bottom symptom.
//
// Behavioural assertion that ties the user's report to the test:
// after setRegion(0, 9) (top half only) + reset via CSI r, the
// cursor + grid must allow plain printing to reach kRows-1.
static void testInv6_BottomRowReachableAfterReset() {
    Harness h;
    h.setRegion(0, 9);
    h.resetRegion();

    EXPECT(h.grid.scrollBottom() == kRows - 1,
           "INV-6: scrollBottom=%d after CSI r reset, expected %d "
           "(matches user-facing symptom: bottom rows unreachable)",
           h.grid.scrollBottom(), kRows - 1);
}

// ----- INV-7 — Public resetScrollRegion() escape hatch.
//
// The Tools → Reset Scroll Region menu action calls grid->
// resetScrollRegion(). This must restore both the active scroll
// region AND the alt-screen saved scroll region to full-screen,
// so an exit-from-alt that follows the menu click doesn't
// re-leak a stale narrowed bound.
static void testInv7_ResetScrollRegionEscapeHatch() {
    Harness h;
    h.setRegion(5, 10);            // narrow main
    h.enterAltScreen();
    h.setRegion(2, 7);             // narrow alt
    h.exitAltScreen();
    EXPECT(h.grid.scrollTop() == 5 && h.grid.scrollBottom() == 10,
           "INV-7 setup: post-alt-exit main region [%d, %d], "
           "expected [5, 10]",
           h.grid.scrollTop(), h.grid.scrollBottom());

    h.grid.resetScrollRegion();    // user clicks the menu

    EXPECT(h.grid.scrollTop() == 0,
           "INV-7: scrollTop=%d after resetScrollRegion(), expected 0",
           h.grid.scrollTop());
    EXPECT(h.grid.scrollBottom() == kRows - 1,
           "INV-7: scrollBottom=%d after resetScrollRegion(), expected %d",
           h.grid.scrollBottom(), kRows - 1);

    // Re-enter alt; alt should also start at full-screen because
    // the saved alt-region was reset too. (Otherwise, the next
    // alt-entry restores the stale narrowed alt region.)
    h.enterAltScreen();
    EXPECT(h.grid.scrollTop() == 0 && h.grid.scrollBottom() == kRows - 1,
           "INV-7: alt-entry after resetScrollRegion() inherited stale "
           "alt region [%d, %d], expected [0, %d]",
           h.grid.scrollTop(), h.grid.scrollBottom(), kRows - 1);
}

int main() {
    testInv1_DecstbmRecorded();
    testInv2_CsiRReset();
    testInv3_RisReset();
    testInv4_AltDecstbmDoesNotLeak();
    testInv5_MainPreEntryRegionPreserved();
    testInv6_BottomRowReachableAfterReset();
    testInv7_ResetScrollRegionEscapeHatch();

    if (g_failures > 0) {
        std::fprintf(stderr, "scroll_region_leak: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "scroll_region_leak: ok\n");
    return 0;
}
