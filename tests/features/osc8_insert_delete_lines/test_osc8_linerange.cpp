// Feature-conformance test for spec.md — asserts OSC 8 hyperlink
// spans track cell rows through CSI L (Insert Line) and CSI M
// (Delete Line). Before the 0.7.7 fix, spans drifted out of sync
// so the underline visually moved with the text but the clickable
// rectangle stayed on the old row.
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>
#include <cstdio>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

struct Harness {
    TerminalGrid grid{kRows, kCols};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

// Move cursor to (row, col) using CSI H (1-based on the wire).
std::string cup(int row, int col) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
    return buf;
}

// Emit an OSC 8 hyperlink wrapping a short label at the current cursor
// position. Uses BEL (0x07) terminator — the canonical xterm form; ST
// (ESC \) works too but some VT parsers (including ours at time of
// writing) split the two-byte terminator across a state transition
// that confuses back-to-back OSC blocks in a single feed() call. BEL
// is single-byte and leaves no ambiguity.
std::string osc8Link(const std::string &url, const std::string &label) {
    return "\033]8;;" + url + "\x07" + label + "\033]8;;\x07";
}

// True iff any span on the given row has a URI matching `url`.
bool rowHasUri(const TerminalGrid &g, int row, const std::string &url) {
    const auto &spans = g.screenHyperlinks(row);
    for (const auto &s : spans) if (s.uri == url) return true;
    return false;
}

int fail(const char *label, const char *detail) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    return 1;
}

}  // namespace

int main() {
    std::setlocale(LC_CTYPE, "");

    // --- I1: CSI L (Insert Line) shifts spans downward ---
    {
        Harness h;
        h.feed(cup(5, 0));
        h.feed(osc8Link("https://i1.example/", "link"));
        if (!rowHasUri(h.grid, 5, "https://i1.example/"))
            return fail("I1 baseline",
                        "span not committed at row 5 before Insert Line");
        // Cursor to row 2, insert 2 lines. Rows 5..(bottom-2) shift down
        // by 2; original row 5 is now at row 7.
        h.feed(cup(2, 0));
        h.feed("\033[2L");
        if (rowHasUri(h.grid, 5, "https://i1.example/"))
            return fail("I1 old row", "span still at row 5 after CSI 2 L");
        if (!rowHasUri(h.grid, 7, "https://i1.example/"))
            return fail("I1 new row",
                        "span did not move to row 7 after CSI 2 L from row 2");
    }

    // --- I2: CSI M (Delete Line) shifts spans upward ---
    {
        Harness h;
        h.feed(cup(5, 0));
        h.feed(osc8Link("https://i2.example/", "link"));
        if (!rowHasUri(h.grid, 5, "https://i2.example/"))
            return fail("I2 baseline",
                        "span not committed at row 5 before Delete Line");
        h.feed(cup(2, 0));
        h.feed("\033[2M");
        if (rowHasUri(h.grid, 5, "https://i2.example/"))
            return fail("I2 old row", "span still at row 5 after CSI 2 M");
        if (!rowHasUri(h.grid, 3, "https://i2.example/"))
            return fail("I2 new row",
                        "span did not move to row 3 after CSI 2 M from row 2");
    }

    // --- I3: span above cursor is untouched by Insert Line ---
    // Insert Line from cursor row 10 affects rows 10..bottom — row 0
    // is outside the affected window and must not move.
    {
        Harness h;
        h.feed(cup(0, 0));
        h.feed(osc8Link("https://i3.example/", "link"));
        if (!rowHasUri(h.grid, 0, "https://i3.example/"))
            return fail("I3 baseline", "span not committed at row 0");
        h.feed(cup(10, 0));
        h.feed("\033[1L");
        if (!rowHasUri(h.grid, 0, "https://i3.example/"))
            return fail("I3 preservation",
                        "span on row 0 disappeared after CSI 1 L at row 10");
    }

    // --- I4: no throw on under-populated hyperlink table ---
    // Covered implicitly by the other scenarios because the guard runs
    // on every call, but keep an explicit smoke run on a fresh grid
    // with no spans ever committed.
    {
        Harness h;
        h.feed(cup(2, 0));
        h.feed("\033[3L");   // Insert 3 lines
        h.feed("\033[3M");   // Delete 3 lines
        // If we reach here without throwing, the guard held.
    }

    std::printf("osc8_insert_delete_lines: 4 invariants held\n");
    return 0;
}
