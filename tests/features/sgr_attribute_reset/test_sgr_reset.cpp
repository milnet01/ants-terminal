// Feature-conformance test for spec.md — asserts every SGR set/reset
// pair produces a clean cell on write-after-reset. Links against
// src/terminalgrid.cpp + src/vtparser.cpp directly.
//
// ANTS-1217 Phase 2: migrated from int main()/exit-code to GoogleTest
// TEST() blocks under Suite SgrAttributeReset.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>
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

void runPair(const Pair &p) {
    Probe probe;
    probe.reset();

    // Set attr, write char, check it's on.
    std::string setSeq = "\x1b[" + std::to_string(p.setCode) + "m";
    probe.feed(setSeq + "X");
    EXPECT_TRUE(probe.cellAt(0, 0).*(p.field))
        << p.name << ": SGR " << p.setCode << " set did not turn attribute on";

    // Reset attr, write char, check it's off.
    std::string resetSeq = "\x1b[" + std::to_string(p.resetCode) + "m";
    probe.feed(resetSeq + "Y");
    EXPECT_FALSE(probe.cellAt(0, 1).*(p.field))
        << p.name << ": SGR " << p.resetCode << " reset left attribute on";
}

}  // namespace

TEST(SgrAttributeReset, Bold) {
    runPair({"bold", 1, 22, &CellAttrs::bold});
}

TEST(SgrAttributeReset, Italic) {
    runPair({"italic", 3, 23, &CellAttrs::italic});
}

TEST(SgrAttributeReset, Underline) {
    runPair({"underline", 4, 24, &CellAttrs::underline});
}

TEST(SgrAttributeReset, Inverse) {
    runPair({"inverse", 7, 27, &CellAttrs::inverse});
}

TEST(SgrAttributeReset, Strikethrough) {
    runPair({"strikethrough", 9, 29, &CellAttrs::strikethrough});
}

TEST(SgrAttributeReset, FullReset) {
    Probe probe;
    probe.reset();

    // Set bold + italic + underline + inverse + strikethrough in one go.
    probe.feed("\x1b[1;3;4;7;9mX");
    const CellAttrs a = probe.cellAt(0, 0);
    EXPECT_TRUE(a.bold && a.italic && a.underline && a.inverse && a.strikethrough)
        << "combined 1;3;4;7;9 did not set all attrs";

    // SGR 0 full reset, then write another char.
    probe.feed("\x1b[0mY");
    const CellAttrs b = probe.cellAt(0, 1);
    EXPECT_FALSE(b.bold || b.italic || b.underline || b.inverse || b.strikethrough)
        << "SGR 0 did not reset all attrs";
}
