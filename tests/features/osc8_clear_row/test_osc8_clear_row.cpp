// Clearing text removes the links on it — see spec.md. ANTS-5076.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>
#include <cstdio>
#include <gtest/gtest.h>
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

std::string cup(int row, int col) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
    return buf;
}

std::string osc8Link(const std::string &url, const std::string &label) {
    return "\033]8;;" + url + "\x07" + label + "\033]8;;\x07";
}

bool rowHasUri(const TerminalGrid &g, int row, const std::string &url) {
    for (const auto &s : g.screenHyperlinks(row))
        if (s.uri == url) return true;
    return false;
}

}  // namespace

// INV-1
TEST(Osc8ClearRow, EraseLineRemovesLinks) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    h.feed(cup(5, 0) + osc8Link("https://a.example/", "link"));
    ASSERT_TRUE(rowHasUri(h.grid, 5, "https://a.example/"));
    h.feed(cup(5, 0) + "\033[2K");
    EXPECT_TRUE(h.grid.screenHyperlinks(5).empty());
}

// INV-2
TEST(Osc8ClearRow, EraseScreenRemovesEveryRowsLinks) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    h.feed(cup(2, 0) + osc8Link("https://b.example/", "one"));
    h.feed(cup(7, 0) + osc8Link("https://c.example/", "two"));
    ASSERT_TRUE(rowHasUri(h.grid, 2, "https://b.example/"));
    ASSERT_TRUE(rowHasUri(h.grid, 7, "https://c.example/"));
    h.feed("\033[2J");
    EXPECT_TRUE(h.grid.screenHyperlinks(2).empty());
    EXPECT_TRUE(h.grid.screenHyperlinks(7).empty());
}

// INV-3
TEST(Osc8ClearRow, PartialEraseKeepsLinkOutsideIt) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    h.feed(cup(4, 0) + osc8Link("https://d.example/", "link"));
    h.feed(cup(4, 10) + "\033[K");
    EXPECT_TRUE(rowHasUri(h.grid, 4, "https://d.example/"));
}

// INV-4
TEST(Osc8ClearRow, PartialEraseRemovesLinkInsideIt) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    h.feed(cup(6, 10) + osc8Link("https://e.example/", "link"));
    ASSERT_TRUE(rowHasUri(h.grid, 6, "https://e.example/"));
    h.feed(cup(6, 20) + "\033[1K");
    EXPECT_FALSE(rowHasUri(h.grid, 6, "https://e.example/"));
}

// INV-5
TEST(Osc8ClearRow, OverprintRemovesLink) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    h.feed(cup(3, 0) + osc8Link("https://f.example/", "link"));
    ASSERT_TRUE(rowHasUri(h.grid, 3, "https://f.example/"));
    h.feed(cup(3, 0) + "XXXX");
    EXPECT_FALSE(rowHasUri(h.grid, 3, "https://f.example/"));
}

// INV-6
TEST(Osc8ClearRow, RedrawnLinkDoesNotAccumulate) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    for (int i = 0; i < 200; ++i)
        h.feed(cup(8, 0) + osc8Link("https://g.example/", "link"));
    EXPECT_EQ(h.grid.screenHyperlinks(8).size(), 1u);
}

// INV-7
TEST(Osc8ClearRow, EmptyLinkAddsNoSpan) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    for (int i = 0; i < 200; ++i)
        h.feed(cup(9, 5) + osc8Link("https://h.example/", ""));
    EXPECT_TRUE(h.grid.screenHyperlinks(9).empty());
}

// INV-8
TEST(Osc8ClearRow, LinkWrappingOffTheBottomRowKeepsItsSpan) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    // Opened five columns from the end of the last row: the label wraps, and
    // the wrap scrolls the screen up one row before the link closes.
    h.feed(cup(kRows - 1, kCols - 5) + osc8Link("https://i.example/", "0123456789"));
    const auto spanOn = [&](int row) -> const HyperlinkSpan * {
        for (const auto &s : h.grid.screenHyperlinks(row))
            if (s.uri == "https://i.example/") return &s;
        return nullptr;
    };
    const HyperlinkSpan *head = spanOn(kRows - 2);
    const HyperlinkSpan *tail = spanOn(kRows - 1);
    ASSERT_NE(head, nullptr) << "INV-8: the label's first half lost its link";
    ASSERT_NE(tail, nullptr) << "INV-8: the label's second half lost its link";
    EXPECT_EQ(head->startCol, kCols - 5);
    EXPECT_EQ(head->endCol, kCols - 1);
    EXPECT_EQ(tail->startCol, 0);
    EXPECT_EQ(tail->endCol, 4);
}
