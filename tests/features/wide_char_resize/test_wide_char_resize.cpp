// Feature-conformance test for spec.md (ANTS-1203) — rewrap
// preserves wide-char cell pairs across resize.
//
// Drives the parser with CJK bytes, pushes them to scrollback,
// then resizes such that the wrap point would split a wide-char
// pair under the pre-fix code. Walks the resized scrollback for
// orphan isWideChar / isWideCont cells.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

#define EXPECT(cond, ...) do {                                  \
    if (!(cond)) {                                              \
        std::fprintf(stderr, "FAIL [%s:%d] ", __FILE__, __LINE__); \
        std::fprintf(stderr, __VA_ARGS__);                      \
        std::fprintf(stderr, "\n");                             \
        ++g_failures;                                           \
    }                                                           \
} while (0)

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

// Walk every scrollback line; flag any orphan wide-char or wide-cont.
//
// Invariant: in a well-formed line, isWideChar at column N is
// immediately followed by isWideCont at column N+1, and no
// isWideCont appears without an isWideChar at column-1.
struct Orphans {
    int orphanLeadCount = 0;
    int orphanContCount = 0;
    int firstOrphanLine = -1;
    int firstOrphanCol = -1;
};

Orphans scanScrollbackForOrphans(const TerminalGrid &grid) {
    Orphans o;
    for (int i = 0; i < grid.scrollbackSize(); ++i) {
        const auto &cells = grid.scrollbackLine(i);
        for (size_t c = 0; c < cells.size(); ++c) {
            const Cell &cell = cells[c];
            if (cell.isWideChar) {
                bool hasCont = (c + 1 < cells.size()) && cells[c + 1].isWideCont;
                if (!hasCont) {
                    if (o.firstOrphanLine < 0) {
                        o.firstOrphanLine = i;
                        o.firstOrphanCol = static_cast<int>(c);
                    }
                    ++o.orphanLeadCount;
                }
            }
            if (cell.isWideCont) {
                bool hasLead = (c > 0) && cells[c - 1].isWideChar;
                if (!hasLead) {
                    if (o.firstOrphanLine < 0) {
                        o.firstOrphanLine = i;
                        o.firstOrphanCol = static_cast<int>(c);
                    }
                    ++o.orphanContCount;
                }
            }
        }
    }
    return o;
}

}  // namespace

// INV-1 — 20-cell wide-char row reflowed to 19 cols preserves pairs.
//
// 10 CJK ideographs at 20 cols fill exactly the row (10 lead + 10
// cont alternating). On resize to 19 cols, the rewrap chunk lands
// at cell 18 (lead of the 10th wide-char, with cont at cell 19) —
// the dangerous boundary. Pre-fix would orphan the cell-18 lead on
// line 0 and the cell-19 cont on line 1.
static void testInv1_TwentyCellRowResizeTo19() {
    Harness h(24, 20);
    // 10 × U+4F60 ("you") = 10 wide chars = 20 cells.
    // U+4F60 in UTF-8: 0xE4 0xBD 0xA0 (3 bytes per char).
    std::string cjk;
    for (int i = 0; i < 10; ++i) cjk += "\xe4\xbd\xa0";
    h.feed(cjk + "\r\n");
    // Push to scrollback by scrolling the screen (24 newlines).
    for (int i = 0; i < 24; ++i) h.feed("\r\n");
    EXPECT(h.grid.scrollbackSize() > 0,
           "INV-1 setup: expected scrollback > 0 after 24+ newlines, "
           "got %d", h.grid.scrollbackSize());

    // The wide-char line should be in scrollback now. Sanity: confirm
    // it has at least one wide-char before the resize triggers rewrap.
    bool foundWideBeforeResize = false;
    for (int i = 0; i < h.grid.scrollbackSize() && !foundWideBeforeResize; ++i) {
        for (const auto &c : h.grid.scrollbackLine(i)) {
            if (c.isWideChar) { foundWideBeforeResize = true; break; }
        }
    }
    EXPECT(foundWideBeforeResize,
           "INV-1 setup: no wide-char cells in scrollback — parser "
           "may not be detecting CJK as wide on this build");

    h.grid.resize(24, 19);

    Orphans o = scanScrollbackForOrphans(h.grid);
    EXPECT(o.orphanLeadCount == 0,
           "INV-1: %d orphan wide-char leads after resize 20→19 "
           "(first at line %d col %d) — rewrap split the pair",
           o.orphanLeadCount, o.firstOrphanLine, o.firstOrphanCol);
    EXPECT(o.orphanContCount == 0,
           "INV-1: %d orphan wide-char continuations after resize",
           o.orphanContCount);
}

// INV-2 — 10-cell wide-char row reflowed to 9 cols preserves pairs.
//
// 5 CJK at 10 cols. Resize to 9 cols. Pre-fix would put 4 wide
// chars + an orphan lead on line 0 (cells 0..8), and orphan cont +
// nothing else on line 1 (cell 0 is cont, no lead).
static void testInv2_TenCellRowResizeTo9() {
    Harness h(24, 10);
    std::string cjk;
    for (int i = 0; i < 5; ++i) cjk += "\xe4\xbd\xa0";  // 5 × U+4F60
    h.feed(cjk + "\r\n");
    for (int i = 0; i < 24; ++i) h.feed("\r\n");
    bool foundWide = false;
    for (int i = 0; i < h.grid.scrollbackSize() && !foundWide; ++i) {
        for (const auto &c : h.grid.scrollbackLine(i)) {
            if (c.isWideChar) { foundWide = true; break; }
        }
    }
    EXPECT(foundWide, "INV-2 setup: no wide-char in scrollback");

    h.grid.resize(24, 9);
    Orphans o = scanScrollbackForOrphans(h.grid);
    EXPECT(o.orphanLeadCount == 0,
           "INV-2: %d orphan leads after resize 10→9 (first at line %d "
           "col %d)",
           o.orphanLeadCount, o.firstOrphanLine, o.firstOrphanCol);
    EXPECT(o.orphanContCount == 0,
           "INV-2: %d orphan conts after resize", o.orphanContCount);
}

int main() {
    // wcwidth() depends on LC_CTYPE; without a UTF-8 locale CJK code
    // points return width 1 instead of 2 and wide-char detection
    // never fires. The main app gets a UTF-8 locale via QApplication
    // initialization; this standalone test must set it explicitly.
    if (!std::setlocale(LC_CTYPE, "C.UTF-8") &&
        !std::setlocale(LC_CTYPE, "en_US.UTF-8") &&
        !std::setlocale(LC_CTYPE, "")) {
        std::fprintf(stderr,
                     "skip: cannot set a UTF-8 locale; wcwidth would "
                     "return 1 for CJK and the test would be vacuous\n");
        return 0;  // skip vs. fail — environment limitation, not a bug
    }
    testInv1_TwentyCellRowResizeTo19();
    testInv2_TenCellRowResizeTo9();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall 2 invariants hold\n");
    return 0;
}
