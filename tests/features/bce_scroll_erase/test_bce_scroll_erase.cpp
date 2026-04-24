// Feature test: Background Color Erase (BCE).
// See spec.md. Drives TerminalGrid through VtParser with `\e[44m` set,
// then fires each erase/scroll operation and asserts the newly-exposed
// cells carry bg == blue (0,0,255 approximated by the ANSI palette).

#include "terminalgrid.h"
#include "vtparser.h"

#include <QColor>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);   \
        ++failures;                                                          \
    }                                                                        \
} while (0)

void feed(TerminalGrid &, VtParser &parser, const std::string &bytes) {
    parser.feed(bytes.data(), static_cast<int>(bytes.size()));
}

VtParser makeParser(TerminalGrid &grid) {
    return VtParser([&grid](const VtAction &a) { grid.processAction(a); });
}

// Resolve SGR 44 (blue) to whatever the grid's palette currently maps it
// to. We don't hardcode (0,0,255) because the grid's ANSI palette is not
// pure blue — it's a themed colour. Capture it after setting SGR.
QColor blueFromSgr44(TerminalGrid &grid, VtParser &parser) {
    // Paint a sentinel cell at (0,0), read back its bg, restore state.
    feed(grid, parser, "\x1b[H\x1b[0m\x1b[44m \x1b[H\x1b[0m");
    return grid.cellAt(0, 0).attrs.bg;
}

bool sameBg(const QColor &a, const QColor &b) {
    if (!a.isValid() || !b.isValid()) return a.isValid() == b.isValid();
    return a.red() == b.red() && a.green() == b.green() && a.blue() == b.blue();
}

// Pretty-print a QColor for diagnostics.
std::string bgStr(const QColor &c) {
    char buf[64];
    if (!c.isValid()) { std::snprintf(buf, sizeof(buf), "<invalid>"); }
    else { std::snprintf(buf, sizeof(buf), "rgb(%d,%d,%d)", c.red(), c.green(), c.blue()); }
    return buf;
}

void testInsertLines(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // Cursor to row 3 (0-based row 2), paint 3 lines, then CSI 3L.
    feed(grid, parser, "\x1b[3;1H\x1b[44m\x1b[3L");
    for (int r = 2; r < 5; ++r) {
        for (int c = 0; c < 80; ++c) {
            if (!sameBg(grid.cellAt(r, c).attrs.bg, blue)) {
                std::fprintf(stderr, "FAIL IL: row %d col %d bg=%s expected %s\n",
                             r, c, bgStr(grid.cellAt(r, c).attrs.bg).c_str(),
                             bgStr(blue).c_str());
                ++failures;
                return;
            }
        }
    }
}

void testDeleteLines(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // CSI 3M deletes 3 lines at cursor; bottom 3 rows (21,22,23) become
    // new blank rows inheriting current bg.
    feed(grid, parser, "\x1b[3;1H\x1b[44m\x1b[3M");
    for (int r = 21; r < 24; ++r) {
        for (int c = 0; c < 80; ++c) {
            if (!sameBg(grid.cellAt(r, c).attrs.bg, blue)) {
                std::fprintf(stderr, "FAIL DL: row %d col %d bg=%s expected %s\n",
                             r, c, bgStr(grid.cellAt(r, c).attrs.bg).c_str(),
                             bgStr(blue).c_str());
                ++failures;
                return;
            }
        }
    }
}

void testScrollUp(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // CSI 1S: scroll up one, new row at bottom (row 23) should be blue.
    feed(grid, parser, "\x1b[44m\x1b[1S");
    for (int c = 0; c < 80; ++c) {
        if (!sameBg(grid.cellAt(23, c).attrs.bg, blue)) {
            std::fprintf(stderr, "FAIL SU: col %d bg=%s expected %s\n",
                         c, bgStr(grid.cellAt(23, c).attrs.bg).c_str(),
                         bgStr(blue).c_str());
            ++failures;
            return;
        }
    }
}

void testScrollDown(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // CSI 1T: scroll down one, new row at top (row 0) should be blue.
    feed(grid, parser, "\x1b[44m\x1b[1T");
    for (int c = 0; c < 80; ++c) {
        if (!sameBg(grid.cellAt(0, c).attrs.bg, blue)) {
            std::fprintf(stderr, "FAIL SD: col %d bg=%s expected %s\n",
                         c, bgStr(grid.cellAt(0, c).attrs.bg).c_str(),
                         bgStr(blue).c_str());
            ++failures;
            return;
        }
    }
}

void testDeleteChars(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // Fill row 0 with 'x', go back to col 0, paint SGR 44, CSI 5P.
    feed(grid, parser, "\x1b[H");
    for (int c = 0; c < 80; ++c) feed(grid, parser, "x");
    feed(grid, parser, "\x1b[H\x1b[44m\x1b[5P");
    // Rightmost 5 cells (cols 75..79) are newly exposed.
    for (int c = 75; c < 80; ++c) {
        if (!sameBg(grid.cellAt(0, c).attrs.bg, blue)) {
            std::fprintf(stderr, "FAIL DCH: col %d bg=%s expected %s\n",
                         c, bgStr(grid.cellAt(0, c).attrs.bg).c_str(),
                         bgStr(blue).c_str());
            ++failures;
            return;
        }
    }
}

void testInsertBlanks(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    feed(grid, parser, "\x1b[H\x1b[44m\x1b[3@");
    for (int c = 0; c < 3; ++c) {
        if (!sameBg(grid.cellAt(0, c).attrs.bg, blue)) {
            std::fprintf(stderr, "FAIL ICH: col %d bg=%s expected %s\n",
                         c, bgStr(grid.cellAt(0, c).attrs.bg).c_str(),
                         bgStr(blue).c_str());
            ++failures;
            return;
        }
    }
}

void testEraseDisplay2(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    feed(grid, parser, "\x1b[44m\x1b[2J");
    for (int r = 0; r < 24; ++r) {
        for (int c = 0; c < 80; ++c) {
            if (!sameBg(grid.cellAt(r, c).attrs.bg, blue)) {
                std::fprintf(stderr, "FAIL ED2: r=%d c=%d bg=%s expected %s\n",
                             r, c, bgStr(grid.cellAt(r, c).attrs.bg).c_str(),
                             bgStr(blue).c_str());
                ++failures;
                return;
            }
        }
    }
}

void testEraseLine0(const QColor &blue) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    feed(grid, parser, "\x1b[5;10H\x1b[44m\x1b[0K");
    // Cursor at (5,10), CSI 0K clears col 9..79 on row 4 (0-based).
    for (int c = 9; c < 80; ++c) {
        if (!sameBg(grid.cellAt(4, c).attrs.bg, blue)) {
            std::fprintf(stderr, "FAIL EL0: col %d bg=%s expected %s\n",
                         c, bgStr(grid.cellAt(4, c).attrs.bg).c_str(),
                         bgStr(blue).c_str());
            ++failures;
            return;
        }
    }
}

void testLfScroll(const QColor &blue, const QColor &defaultBg) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // Park cursor at bottom row (row 24 = row 23 0-based), paint bg, LF.
    // LF at bottom scrolls up; new bottom row should be blue.
    feed(grid, parser, "\x1b[24;1H\x1b[44m\n");
    for (int c = 0; c < 80; ++c) {
        if (!sameBg(grid.cellAt(23, c).attrs.bg, blue)) {
            std::fprintf(stderr, "FAIL LF-scroll: col %d bg=%s expected %s\n",
                         c, bgStr(grid.cellAt(23, c).attrs.bg).c_str(),
                         bgStr(blue).c_str());
            ++failures;
            return;
        }
    }
    (void)defaultBg;
}

void testSgrResetBeforeErase(const QColor &defaultBg) {
    TerminalGrid grid(24, 80);
    VtParser parser = makeParser(grid);
    // CSI 0m drops SGR; subsequent CSI 2J should fall back to default bg.
    feed(grid, parser, "\x1b[44m\x1b[0m\x1b[2J");
    if (!sameBg(grid.cellAt(0, 0).attrs.bg, defaultBg)) {
        std::fprintf(stderr, "FAIL SGR-reset: bg=%s expected %s\n",
                     bgStr(grid.cellAt(0, 0).attrs.bg).c_str(),
                     bgStr(defaultBg).c_str());
        ++failures;
    }
}

} // namespace

int main() {
    TerminalGrid probe(4, 4);
    VtParser probeParser = makeParser(probe);
    const QColor blue = blueFromSgr44(probe, probeParser);
    const QColor defaultBg = probe.defaultBg();

    CHECK(blue.isValid(), "SGR 44 did not resolve to a valid bg color");
    CHECK(!sameBg(blue, defaultBg),
          "SGR 44 resolved to default bg — palette or probe is wrong");

    testInsertLines(blue);
    testDeleteLines(blue);
    testScrollUp(blue);
    testScrollDown(blue);
    testDeleteChars(blue);
    testInsertBlanks(blue);
    testEraseDisplay2(blue);
    testEraseLine0(blue);
    testLfScroll(blue, defaultBg);
    testSgrResetBeforeErase(defaultBg);

    if (failures == 0) {
        std::fprintf(stderr, "PASS bce_scroll_erase (10 subcases)\n");
        return 0;
    }
    std::fprintf(stderr, "bce_scroll_erase: %d failure(s)\n", failures);
    return 1;
}
