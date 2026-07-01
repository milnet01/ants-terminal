// Feature-conformance test for spec.md — ANTS-1334. Asserts that
// zero-width combiners attach to the LEAD cell of a wide char,
// even at the right-edge (delayed-wrap) position.
//
// Exit 0 = all invariants hold. Non-zero = regression.

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

// UTF-8 encode a codepoint into the byte string fed to the parser.
std::string utf8(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

bool combiningAt(const TerminalGrid &g, int row, int col, uint32_t cp) {
    const auto &cmap = g.screenCombining(row);
    auto it = cmap.find(col);
    if (it == cmap.end()) return false;
    for (uint32_t v : it->second) if (v == cp) return true;
    return false;
}

void fail(const char *label, const char *detail){
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    ADD_FAILURE() << label << ": " << detail;
}

}  // namespace

TEST(CombiningAtWideRightEdge, Main) {
    std::setlocale(LC_CTYPE, "en_US.UTF-8");

    constexpr uint32_t kCJK = 0x4E2D;       // 中 (wide)
    constexpr uint32_t kAccent = 0x0301;    // COMBINING ACUTE ACCENT

    // --- INV-1: right-edge delayed-wrap case (cursorCol cols-2) ---
    {
        Harness h;
        h.feed(cup(0, kCols - 2));   // cursor at col 78 of 80
        h.feed(utf8(kCJK));          // wide char: lead 78, cont 79, wrapNext=true
        h.feed(utf8(kAccent));       // combiner
        if (!combiningAt(h.grid, 0, kCols - 2, kAccent))
            fail("INV-1 lead attach",
                        "combiner did not attach to lead col cols-2 "
                        "after right-edge wide-char + combiner sequence");
        if (combiningAt(h.grid, 0, kCols - 1, kAccent))
            fail("INV-1 cont leak",
                        "combiner leaked onto continuation col cols-1");
    }

    // --- INV-3: interior position (no regression) ---
    {
        Harness h;
        h.feed(cup(0, 10));
        h.feed(utf8(kCJK));          // wide char: lead 10, cont 11
        h.feed(utf8(kAccent));
        if (!combiningAt(h.grid, 0, 10, kAccent))
            fail("INV-3 interior lead",
                        "interior combiner missing on lead col 10");
        if (combiningAt(h.grid, 0, 11, kAccent))
            fail("INV-3 interior cont leak",
                        "combiner leaked onto continuation col 11");
    }

    // --- INV-3 second branch: non-wrapNext + cursorCol > 0 ---
    // Write narrow char + combiner — the m_wrapNext=false / m_cursorCol-1
    // branch is exercised; targetCol points at a non-wide cell.
    {
        Harness h;
        h.feed(cup(0, 5));
        h.feed("a");                 // narrow char at col 5
        h.feed(utf8(kAccent));       // combiner → col 5 (cursorCol-1)
        if (!combiningAt(h.grid, 0, 5, kAccent))
            fail("INV-3 non-wrapNext narrow",
                        "narrow-char combiner branch broken");
    }

    std::printf("combining_at_wide_right_edge: "
                "3 invariants held\n");
}
