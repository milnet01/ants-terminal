// ANTS-6703 — scrollback rows drop their blank tails. Contract: spec.md
// beside this file.

#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

#ifndef SRC_TERMINALGRID_CPP_PATH
#  error "SRC_TERMINALGRID_CPP_PATH compile definition required"
#endif

namespace {

constexpr int kRows = 4;
constexpr int kCols = 20;

struct Term {
    TerminalGrid grid{kRows, kCols};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }
    // Push everything on screen into scrollback.
    void flush() { for (int i = 0; i < kRows; ++i) feed("\r\n"); }
};

std::string text(const std::vector<Cell> &cells) {
    std::string s;
    for (const Cell &c : cells) s += c.codepoint < 0x80 ? char(c.codepoint) : '?';
    return s;
}

int findLine(const TerminalGrid &g, const std::string &prefix) {
    for (int i = 0; i < g.scrollbackSize(); ++i)
        if (text(g.scrollbackLine(i)).rfind(prefix, 0) == 0) return i;
    return -1;
}

}  // namespace

TEST(ScrollbackTrimBlankTails, Inv1PlainTailDropped) {
    Term t;
    t.feed("abc\r\n");
    t.flush();
    const int i = findLine(t.grid, "abc");
    ASSERT_GE(i, 0);
    EXPECT_EQ(t.grid.scrollbackLine(i).size(), 3u);
    EXPECT_FALSE(t.grid.scrollbackLineWrapped(i));
}

TEST(ScrollbackTrimBlankTails, Inv2VisibleTailKept) {
    Term t;
    t.feed("\x1b[44mbg\x1b[K\x1b[0m\r\n");            // blue to end of line
    t.feed("\x1b[4mul   \x1b[0m\r\n");                 // underlined spaces
    t.flush();
    const int bg = findLine(t.grid, "bg");
    const int ul = findLine(t.grid, "ul");
    ASSERT_GE(bg, 0);
    ASSERT_GE(ul, 0);
    EXPECT_EQ(t.grid.scrollbackLine(bg).size(), size_t(kCols));
    EXPECT_EQ(t.grid.scrollbackLine(ul).size(), 5u);
}

TEST(ScrollbackTrimBlankTails, Inv3SoftWrappedRowKeptWhole) {
    Term t;
    t.feed(std::string(kCols - 1, 'x') + " tail\r\n");  // space at the last column
    t.flush();
    const int i = findLine(t.grid, std::string(kCols - 1, 'x'));
    ASSERT_GE(i, 0);
    EXPECT_TRUE(t.grid.scrollbackLineWrapped(i));
    EXPECT_EQ(t.grid.scrollbackLine(i).size(), size_t(kCols));
}

TEST(ScrollbackTrimBlankTails, Inv4ResizeKeepsRowsTrimmedAndTextIntact) {
    Term t;
    t.feed("abc\r\n");
    t.feed(std::string(kCols - 1, 'x') + " tail\r\n");
    t.flush();
    t.grid.resize(kRows, kCols + 30);
    const int abc = findLine(t.grid, "abc");
    ASSERT_GE(abc, 0);
    EXPECT_EQ(t.grid.scrollbackLine(abc).size(), 3u) << "a widening resize padded the row";
    const int joined = findLine(t.grid, std::string(kCols - 1, 'x'));
    ASSERT_GE(joined, 0);
    EXPECT_EQ(text(t.grid.scrollbackLine(joined)), std::string(kCols - 1, 'x') + " tail");
}

TEST(ScrollbackTrimBlankTails, Inv5LeftoverBufferReturnsToPool) {
    const std::string fn = ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_TERMINALGRID_CPP_PATH, "TermLine TerminalGrid::trimmedForScrollback("));
    EXPECT_NE(fn.find("returnCellsRow("), std::string::npos);
}
