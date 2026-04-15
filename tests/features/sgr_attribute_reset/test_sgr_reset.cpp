// Feature-conformance test for spec.md — asserts every SGR set/reset
// pair produces a clean cell on write-after-reset. Links against
// src/terminalgrid.cpp + src/vtparser.cpp directly.
//
// Exit 0 = all pairs + SGR 0 full-reset pass. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

// Thin harness: fresh grid, feed escape sequences, read back the most
// recently written cell's attrs. Returns the cell at row 0, col 0 after
// all feeds — we reset cursor before each sub-test so the cell write
// lands at a predictable location.
struct Probe {
    TerminalGrid grid;
    VtParser parser;

    Probe()
        : grid(kRows, kCols),
          parser([this](const VtAction &a) { grid.processAction(a); }) {}

    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }

    void reset() {
        // ESC [ H = cursor home; ESC [ 2J = clear; ESC [ 0 m = attrs reset.
        feed("\x1b[H\x1b[2J\x1b[0m");
    }

    CellAttrs cellAt(int row, int col) {
        return grid.cellAt(row, col).attrs;
    }
};

// One test pair: setCode, resetCode, human-readable attr name, and a
// pointer to the CellAttrs field being checked.
struct Pair {
    const char *name;
    int setCode;
    int resetCode;
    bool CellAttrs::*field;    // member pointer into CellAttrs
};

int runPair(const Pair &p) {
    Probe probe;
    probe.reset();

    // Set attr, write char, check it's on.
    std::string setSeq = "\x1b[" + std::to_string(p.setCode) + "m";
    probe.feed(setSeq + "X");
    bool afterSet = probe.cellAt(0, 0).*(p.field);

    // Reset attr, write char, check it's off.
    std::string resetSeq = "\x1b[" + std::to_string(p.resetCode) + "m";
    probe.feed(resetSeq + "Y");
    bool afterReset = probe.cellAt(0, 1).*(p.field);

    const bool ok = afterSet && !afterReset;
    std::fprintf(stderr,
                 "[%-15s] SGR %d set -> %s | SGR %d reset -> %s  %s\n",
                 p.name, p.setCode, afterSet ? "ON " : "OFF",
                 p.resetCode, afterReset ? "ON " : "OFF",
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int runFullReset() {
    Probe probe;
    probe.reset();

    // Set bold + italic + underline + inverse + strikethrough in one go.
    probe.feed("\x1b[1;3;4;7;9mX");
    const CellAttrs a = probe.cellAt(0, 0);
    const bool setAll = a.bold && a.italic && a.underline && a.inverse &&
                        a.strikethrough;

    // SGR 0 full reset, then write another char.
    probe.feed("\x1b[0mY");
    const CellAttrs b = probe.cellAt(0, 1);
    const bool resetAll = !b.bold && !b.italic && !b.underline && !b.inverse &&
                          !b.strikethrough;

    const bool ok = setAll && resetAll;
    std::fprintf(stderr,
                 "[%-15s] combined 1;3;4;7;9 set -> %s | SGR 0 reset -> %s  %s\n",
                 "full_reset",
                 setAll ? "all ON " : "partial",
                 resetAll ? "all OFF" : "partial",
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main() {
    const Pair pairs[] = {
        {"bold",          1, 22, &CellAttrs::bold},
        {"italic",        3, 23, &CellAttrs::italic},
        {"underline",     4, 24, &CellAttrs::underline},
        {"inverse",       7, 27, &CellAttrs::inverse},
        {"strikethrough", 9, 29, &CellAttrs::strikethrough},
    };

    int failures = 0;
    for (const auto &p : pairs) failures += runPair(p);
    failures += runFullReset();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d SGR pair(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
