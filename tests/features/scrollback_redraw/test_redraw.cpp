// Feature-conformance test for spec.md — asserts that a main-screen
// CSI 2J + repaint does not grow scrollback by more than rows + slack.
//
// Links against src/terminalgrid.cpp and src/vtparser.cpp directly (see
// CMakeLists.txt — this is a white-box test in the sense that it builds
// against the same source objects the application does, but it exercises
// them only through public API, so the test survives refactors that
// don't change the public behavior).
//
// Exit 0 = invariant holds. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <string>

namespace {

// Phase count of lines printed before/after CSI 2J. Must be > rows so
// the screen fills and scrollback actually grows during phase 1 — if
// L1 <= rows, nothing ever pushes to scrollback and the invariant
// would pass trivially.
constexpr int kRows = 24;
constexpr int kCols = 80;
constexpr int kLinesPerPhase = 100;   // L1 = L2 = 100, both > kRows.

int runScenario(const char *scenarioName, const char *clearSeq, bool repaintIdenticalContent) {
    TerminalGrid grid(kRows, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });

    auto feed = [&](const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    };

    // Phase 1 — initial paint. Fills screen and pushes (L1 - rows) lines
    // into scrollback via the usual newLine → scrollUp path.
    for (int i = 0; i < kLinesPerPhase; ++i) {
        feed("phase1 line " + std::to_string(i) + "\r\n");
    }
    const int sbAfterPhase1 = grid.scrollbackSize();

    // Phase 2 — simulate a TUI repaint. `clearSeq` is the erase-and-home
    // sequence under test — the canonical pattern is `CSI 2J; CSI H`, but
    // some TUIs use equivalent-effect variants like `CSI H; CSI 0J` which
    // produces the same post-state and therefore must suppress the same way.
    // Without the suppression window, each newline during this phase pushes
    // a line to scrollback, doubling it.
    feed(clearSeq);
    for (int i = 0; i < kLinesPerPhase; ++i) {
        if (repaintIdenticalContent)
            feed("phase1 line " + std::to_string(i) + "\r\n");  // same as phase 1
        else
            feed("phase2 line " + std::to_string(i) + "\r\n");  // diverged content
    }
    const int sbAfterPhase2 = grid.scrollbackSize();

    const int growth = sbAfterPhase2 - sbAfterPhase1;
    const int threshold = kRows + 10;   // see spec.md §Invariant

    std::fprintf(stderr,
                 "[%s] scrollback: phase1=%d  phase2=%d  growth=%d  threshold=%d  %s\n",
                 scenarioName, sbAfterPhase1, sbAfterPhase2, growth, threshold,
                 (growth <= threshold ? "PASS" : "FAIL"));

    if (growth > threshold) {
        std::fprintf(stderr,
                     "  FAIL: main-screen CSI 2J repaint was NOT suppressed — "
                     "scrollback grew by %d lines across the repaint burst, "
                     "indicating each newline during the repaint pushed to "
                     "scrollback. See spec.md §Contract.\n",
                     growth);
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    // Canonical + equivalent-effect variants. Claude Code uses CSI 2J + CSI H
    // (the first two); some TUIs and older terminals use CSI H + CSI 0J
    // (move home, erase to end — same post-state as CSI 2J from home).
    // CSI 1J from the bottom-right corner produces the same post-state too,
    // though in practice no TUI emits that pattern — guarded for correctness.
    int failures = 0;
    failures += runScenario("identical-repaint-2J",   "\x1b[2J\x1b[H",         /*identical=*/true);
    failures += runScenario("diverged-repaint-2J",    "\x1b[2J\x1b[H",         /*identical=*/false);
    failures += runScenario("identical-repaint-0J",   "\x1b[H\x1b[0J",         /*identical=*/true);
    failures += runScenario("identical-repaint-1J",   "\x1b[24;80H\x1b[1J\x1b[H", /*identical=*/true);

    if (failures > 0) {
        std::fprintf(stderr, "\n%d scenario(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
