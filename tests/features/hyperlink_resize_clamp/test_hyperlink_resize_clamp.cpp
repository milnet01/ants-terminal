// Feature-conformance test for spec.md — asserts an open-but-uncommitted
// OSC 8 hyperlink survives a shrink resize. Pre-0.7.7, the stored
// start row/col were not clamped; after the resize the row index
// became out-of-range, and the close guard at handleOsc silently
// dropped the span. Post-fix, the coordinates are clamped on resize
// so the span commits at a valid row.
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>
#include <cstdio>
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

int fail(const char *label, const char *detail) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    return 1;
}

// Scan the first `rows` rows of the grid's hyperlink table for any
// span whose URI contains `needle`. Returns the row it was found on,
// or -1 if absent.
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

int main() {
    std::setlocale(LC_CTYPE, "");

    // Scenario: open hyperlink at (20, 50), shrink grid to (5, 10),
    // close hyperlink. Post-fix: span must land on a row in [0, 4].
    {
        Harness h;
        h.feed(cup(20, 50));
        // Open OSC 8 but do NOT close yet. BEL terminator — ST (ESC \)
        // can interact oddly when the following feed is also an OSC.
        h.feed("\033]8;;https://clamped.example/\x07");
        h.feed("text");
        // Shrink: 20 > 4 so m_hyperlinkStartRow must clamp.
        h.grid.resize(5, 10);
        // Now close the hyperlink.
        h.feed("\033]8;;\x07");

        int row = findUriRow(h.grid, 5, "clamped.example");
        if (row < 0)
            return fail("I1/I2 shrink",
                        "closed hyperlink did not land on any valid row — "
                        "pre-0.7.7 drop behaviour still live?");
        if (row > 4)
            return fail("I1/I2 shrink",
                        "hyperlink row index exceeds new grid height");
    }

    // Scenario: identical setup but shrink that keeps the open row
    // in range — the clamp must still not throw, and the span must
    // commit on its original row.
    {
        Harness h;
        h.feed(cup(3, 5));
        h.feed("\033]8;;https://inrange.example/\x07");
        h.feed("text");
        h.grid.resize(10, 20);  // cursor row 3 stays valid
        h.feed("\033]8;;\x07");
        int row = findUriRow(h.grid, 10, "inrange.example");
        if (row != 3)
            return fail("I3 no-op resize",
                        "hyperlink moved off its original row after a "
                        "resize that didn't require clamping");
    }

    std::printf("hyperlink_resize_clamp: shrink clamp + no-op resize both hold\n");
    return 0;
}
