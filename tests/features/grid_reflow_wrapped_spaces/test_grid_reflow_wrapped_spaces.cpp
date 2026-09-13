// Re-wrapping keeps the spaces inside a wrapped line — see spec.md.
// ANTS-5076.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>
#include <gtest/gtest.h>
#include <string>

namespace {

struct Harness {
    TerminalGrid grid;
    VtParser parser;
    Harness(int rows, int cols)
        : grid(rows, cols),
          parser([this](const VtAction &a) { grid.processAction(a); }) {}
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

std::string rowText(const TerminalGrid &g, int row, int cols) {
    std::string out;
    for (int c = 0; c < cols; ++c)
        out += static_cast<char>(g.cellAt(row, c).codepoint);
    return out;
}

}  // namespace

// INV-1
TEST(GridReflowWrappedSpaces, SpaceAtWrapPointSurvives) {
    std::setlocale(LC_CTYPE, "");
    Harness h(5, 10);
    h.feed("abcdefghi jklm");
    ASSERT_EQ(rowText(h.grid, 0, 10), "abcdefghi ");
    h.grid.resize(5, 20);
    EXPECT_EQ(rowText(h.grid, 0, 14), "abcdefghi jklm");
}

// INV-2
TEST(GridReflowWrappedSpaces, LogicalLineTailStillTrimmed) {
    std::setlocale(LC_CTYPE, "");
    Harness h(5, 10);
    h.feed("ab\r\ncd");
    h.grid.resize(5, 5);
    EXPECT_EQ(h.grid.cellAt(1, 0).codepoint, static_cast<uint32_t>('c'));
}
