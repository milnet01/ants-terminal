// Feature-conformance test for spec.md — locks the observable behavior
// of TerminalGrid::scrollUp / scrollDown so the `std::rotate` refactor
// (0.7.9) can't silently regress. See spec.md for the invariants.
//
// scrollUp / scrollDown / setScrollRegion are private — we exercise them
// via their CSI dispatchers (CSI N S = scrollUp, CSI N T = scrollDown,
// CSI top;bottom r = DECSTBM setScrollRegion).
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kRows = 10;
constexpr int kCols = 16;

int g_failures = 0;

#define EXPECT(cond, ...) do {                                \
    if (!(cond)) {                                            \
        std::fprintf(stderr, "FAIL [%s:%d] ", __FILE__, __LINE__); \
        std::fprintf(stderr, __VA_ARGS__);                    \
        std::fprintf(stderr, "\n");                           \
        ++g_failures;                                         \
    }                                                         \
} while (0)

struct Harness {
    TerminalGrid grid;
    VtParser parser;
    Harness() : grid(kRows, kCols), parser([this](const VtAction &a) { grid.processAction(a); }) {}
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }
    void setRegion(int topRow, int bottomRow) {  // 0-indexed
        feed("\x1b[" + std::to_string(topRow + 1) + ";" +
             std::to_string(bottomRow + 1) + "r");
    }
    void resetRegion() { feed("\x1b[r"); }
    void scrollUp(int n)   { feed("\x1b[" + std::to_string(n) + "S"); }
    void scrollDown(int n) { feed("\x1b[" + std::to_string(n) + "T"); }
    void labelRows() {
        for (int r = 0; r < kRows; ++r) {
            std::string tag = "row" + std::to_string(r);
            feed("\x1b[" + std::to_string(r + 1) + ";1H" + tag);
        }
    }
};

std::string rowText(const TerminalGrid &g, int row) {
    std::string s;
    for (int c = 0; c < kCols; ++c) {
        uint32_t cp = g.cellAt(row, c).codepoint;
        s.push_back((cp >= 0x20 && cp < 0x7f) ? static_cast<char>(cp) : '.');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string scrollbackRowText(const TerminalGrid &g, int idx) {
    std::string s;
    for (const auto &cell : g.scrollbackLine(idx)) {
        uint32_t cp = cell.codepoint;
        s.push_back((cp >= 0x20 && cp < 0x7f) ? static_cast<char>(cp) : '.');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool rowIsBlank(const TerminalGrid &g, int row) {
    for (int c = 0; c < kCols; ++c) {
        if (g.cellAt(row, c).codepoint != ' ') return false;
    }
    return true;
}

// ----- I1 / I3 — scrollUp inside a partial region, rows outside untouched.
void testPartialRegionScrollUp() {
    Harness h;
    h.labelRows();
    h.setRegion(3, 7);
    h.scrollUp(2);

    for (int r : {0, 1, 2, 8, 9}) {
        std::string expected = "row" + std::to_string(r);
        EXPECT(rowText(h.grid, r) == expected,
               "I3: row %d changed: expected '%s' got '%s'",
               r, expected.c_str(), rowText(h.grid, r).c_str());
    }

    EXPECT(rowText(h.grid, 3) == "row5", "I1: row 3 got '%s', want 'row5'", rowText(h.grid, 3).c_str());
    EXPECT(rowText(h.grid, 4) == "row6", "I1: row 4 got '%s', want 'row6'", rowText(h.grid, 4).c_str());
    EXPECT(rowText(h.grid, 5) == "row7", "I1: row 5 got '%s', want 'row7'", rowText(h.grid, 5).c_str());

    EXPECT(rowIsBlank(h.grid, 6), "I1: row 6 not blank: '%s'", rowText(h.grid, 6).c_str());
    EXPECT(rowIsBlank(h.grid, 7), "I1: row 7 not blank: '%s'", rowText(h.grid, 7).c_str());

    EXPECT(h.grid.scrollbackSize() == 0,
           "I5: scrollback grew from partial-region scrollUp: %d",
           h.grid.scrollbackSize());
}

// ----- I2 / I3 — scrollDown inside a partial region.
void testPartialRegionScrollDown() {
    Harness h;
    h.labelRows();
    h.setRegion(3, 7);
    h.scrollDown(2);

    for (int r : {0, 1, 2, 8, 9}) {
        std::string expected = "row" + std::to_string(r);
        EXPECT(rowText(h.grid, r) == expected,
               "I3: row %d changed: expected '%s' got '%s'",
               r, expected.c_str(), rowText(h.grid, r).c_str());
    }

    EXPECT(rowText(h.grid, 5) == "row3", "I2: row 5 got '%s', want 'row3'", rowText(h.grid, 5).c_str());
    EXPECT(rowText(h.grid, 6) == "row4", "I2: row 6 got '%s', want 'row4'", rowText(h.grid, 6).c_str());
    EXPECT(rowText(h.grid, 7) == "row5", "I2: row 7 got '%s', want 'row5'", rowText(h.grid, 7).c_str());

    EXPECT(rowIsBlank(h.grid, 3), "I2: row 3 not blank: '%s'", rowText(h.grid, 3).c_str());
    EXPECT(rowIsBlank(h.grid, 4), "I2: row 4 not blank: '%s'", rowText(h.grid, 4).c_str());
}

// ----- I4 — main screen, scrollTop==0 pushes to scrollback.
void testMainScreenScrollUpPushesScrollback() {
    Harness h;
    h.labelRows();
    // Default scroll region covers the full screen (top 0).
    const int sbBefore = h.grid.scrollbackSize();
    h.scrollUp(3);

    const int pushed = h.grid.scrollbackSize() - sbBefore;
    EXPECT(pushed == 3, "I4: expected 3 rows pushed, got %d", pushed);

    if (pushed == 3) {
        const int base = h.grid.scrollbackSize() - 3;
        EXPECT(scrollbackRowText(h.grid, base + 0) == "row0",
               "I4: scrollback[0] got '%s' want 'row0'", scrollbackRowText(h.grid, base + 0).c_str());
        EXPECT(scrollbackRowText(h.grid, base + 1) == "row1",
               "I4: scrollback[1] got '%s' want 'row1'", scrollbackRowText(h.grid, base + 1).c_str());
        EXPECT(scrollbackRowText(h.grid, base + 2) == "row2",
               "I4: scrollback[2] got '%s' want 'row2'", scrollbackRowText(h.grid, base + 2).c_str());
    }

    for (int i = 0; i <= 6; ++i) {
        std::string expected = "row" + std::to_string(i + 3);
        EXPECT(rowText(h.grid, i) == expected,
               "I1 (full-screen): row %d got '%s' want '%s'",
               i, rowText(h.grid, i).c_str(), expected.c_str());
    }
    for (int i = 7; i <= 9; ++i) {
        EXPECT(rowIsBlank(h.grid, i), "I1: row %d not blank: '%s'", i, rowText(h.grid, i).c_str());
    }
}

// ----- I6 — alt screen scrollUp must not push to scrollback.
void testAltScreenScrollUpNoScrollback() {
    Harness h;
    h.labelRows();
    h.feed("\x1b[?1049h");  // enter alt screen
    EXPECT(h.grid.altScreenActive(), "I6: alt screen should be active after DECSET 1049");

    // Label the alt-screen rows so scrollUp has real content to move.
    for (int r = 0; r < kRows; ++r) {
        h.feed("\x1b[" + std::to_string(r + 1) + ";1H" + "alt" + std::to_string(r));
    }

    const int sbBefore = h.grid.scrollbackSize();
    h.scrollUp(3);
    const int pushed = h.grid.scrollbackSize() - sbBefore;
    EXPECT(pushed == 0, "I6: alt-screen scrollUp pushed %d rows to scrollback", pushed);
}

// ----- I7 — hyperlinks follow their row.
void testHyperlinksFollowRow() {
    Harness h;
    h.labelRows();
    h.grid.addRowHyperlink(5, 0, 3, QStringLiteral("https://example.com/a"));
    h.grid.addRowHyperlink(8, 0, 3, QStringLiteral("https://example.com/b"));

    EXPECT(h.grid.screenHyperlinks(5).size() == 1, "I7 setup: row 5 should have 1 hyperlink");
    EXPECT(h.grid.screenHyperlinks(8).size() == 1, "I7 setup: row 8 should have 1 hyperlink");

    h.setRegion(3, 7);
    h.scrollUp(2);

    EXPECT(h.grid.screenHyperlinks(3).size() == 1,
           "I7: row 3 should have 1 hyperlink after shift, got %zu",
           h.grid.screenHyperlinks(3).size());
    if (!h.grid.screenHyperlinks(3).empty()) {
        EXPECT(h.grid.screenHyperlinks(3)[0].uri == "https://example.com/a",
               "I7: shifted hyperlink uri got '%s'",
               h.grid.screenHyperlinks(3)[0].uri.c_str());
    }
    EXPECT(h.grid.screenHyperlinks(5).empty(),
           "I7: row 5 should have no hyperlinks after shift, got %zu",
           h.grid.screenHyperlinks(5).size());
    EXPECT(h.grid.screenHyperlinks(8).size() == 1,
           "I7: row 8 (outside region) hyperlink count changed: got %zu",
           h.grid.screenHyperlinks(8).size());
    if (!h.grid.screenHyperlinks(8).empty()) {
        EXPECT(h.grid.screenHyperlinks(8)[0].uri == "https://example.com/b",
               "I7: row 8 hyperlink uri got '%s'",
               h.grid.screenHyperlinks(8)[0].uri.c_str());
    }
}

// ----- I8 — count > regionHeight is clamped (whole region blank).
void testCountOverflowClamped() {
    {
        Harness h;
        h.labelRows();
        h.setRegion(2, 6);   // height 5
        h.scrollUp(50);

        for (int r = 2; r <= 6; ++r) {
            EXPECT(rowIsBlank(h.grid, r), "I8-up: row %d not blank after overflow scrollUp: '%s'",
                   r, rowText(h.grid, r).c_str());
        }
        for (int r : {0, 1, 7, 8, 9}) {
            std::string expected = "row" + std::to_string(r);
            EXPECT(rowText(h.grid, r) == expected,
                   "I8-up: row %d outside region changed: got '%s' want '%s'",
                   r, rowText(h.grid, r).c_str(), expected.c_str());
        }
        EXPECT(h.grid.scrollbackSize() == 0,
               "I8-up: partial region scrollUp leaked to scrollback: %d rows",
               h.grid.scrollbackSize());
    }
    {
        Harness h;
        h.labelRows();
        h.setRegion(2, 6);
        h.scrollDown(50);
        for (int r = 2; r <= 6; ++r) {
            EXPECT(rowIsBlank(h.grid, r), "I8-down: row %d not blank: '%s'",
                   r, rowText(h.grid, r).c_str());
        }
        for (int r : {0, 1, 7, 8, 9}) {
            std::string expected = "row" + std::to_string(r);
            EXPECT(rowText(h.grid, r) == expected,
                   "I8-down: row %d outside region changed: got '%s' want '%s'",
                   r, rowText(h.grid, r).c_str(), expected.c_str());
        }
    }
}

}  // namespace

int main() {
    testPartialRegionScrollUp();
    testPartialRegionScrollDown();
    testMainScreenScrollUpPushesScrollback();
    testAltScreenScrollUpNoScrollback();
    testHyperlinksFollowRow();
    testCountOverflowClamped();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "All scroll-region rotate invariants hold.\n");
    return 0;
}
