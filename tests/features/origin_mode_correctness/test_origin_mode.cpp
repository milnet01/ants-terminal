// Feature test: DECOM origin-mode CUP/HVP/VPA translation, DECSTBM
// home, and DECSC/DECRC saving DECOM + DECAWM. See spec.md.
//
// Pre-fix invariants 2-12 all FAIL — CUP doesn't translate, DECSC
// doesn't save the flags. Post-fix all 12 pass.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct Harness {
    TerminalGrid grid{24, 80};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};

    void feed(const char *s) {
        parser.feed(s, static_cast<int>(std::strlen(s)));
    }
    int row() const { return grid.cursorRow(); }
    int col() const { return grid.cursorCol(); }
};

int g_failures = 0;
void fail(const char *label, const char *detail) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    ++g_failures;
}

void expectPos(const char *label, Harness &h, int wantRow, int wantCol) {
    if (h.row() != wantRow || h.col() != wantCol) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "expected cursor (%d,%d), got (%d,%d)",
                      wantRow, wantCol, h.row(), h.col());
        fail(label, buf);
    }
}

std::string readFile(const char *path) {
    std::ifstream f(path);
    if (!f.good()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main() {
    // ---------- I1: DECOM off, CUP absolute (sanity baseline) ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");        // scroll region rows 4..10 (1-based 5..11)
        h.feed("\x1b[5;3H");         // CUP (5,3) → row 4, col 2
        expectPos("I1", h, 4, 2);
    }

    // ---------- I2: DECOM on, CUP 1;1H lands at scrollTop ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");          // DECOM on
        h.feed("\x1b[1;1H");
        expectPos("I2", h, 4, 0);
    }

    // ---------- I3: DECOM on, CUP 3;5H = (scrollTop+2, 4) ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");
        h.feed("\x1b[3;5H");
        expectPos("I3", h, 6, 4);
    }

    // ---------- I4: DECOM on, CUP 99;1H clamps to scrollBottom ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");
        h.feed("\x1b[99;1H");
        expectPos("I4", h, 10, 0);
    }

    // ---------- I5: HVP ('f') matches CUP semantics ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");
        h.feed("\x1b[99;1f");
        expectPos("I5", h, 10, 0);
    }

    // ---------- I6: VPA 'd' origin-mode translates ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");
        h.feed("\x1b[1;5H");         // park cursor with col=4
        h.feed("\x1b[3d");           // VPA row 3 (origin-relative)
        expectPos("I6", h, 6, 4);
    }

    // ---------- I7: VPA clamps to scrollBottom under DECOM ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");
        h.feed("\x1b[99d");
        if (h.row() != 10)
            fail("I7", "VPA 99 with DECOM on did not clamp to scrollBottom 10");
    }

    // ---------- I8: DECSTBM home is (0,0) without DECOM ----------
    {
        Harness h;
        h.feed("\x1b[10;20H");       // park cursor away
        h.feed("\x1b[5;11r");        // DECSTBM
        expectPos("I8", h, 0, 0);
    }

    // ---------- I9: DECSTBM home is (scrollTop, 0) with DECOM ----------
    {
        Harness h;
        h.feed("\x1b[?6h");          // DECOM on FIRST so DECSTBM picks it up
        h.feed("\x1b[10;20H");
        h.feed("\x1b[5;11r");        // DECSTBM
        expectPos("I9", h, 4, 0);
    }

    // ---------- I10: DECSC (CSI s) preserves DECOM across DECRC ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");          // DECOM on
        h.feed("\x1b[s");            // DECSC
        h.feed("\x1b[?6l");          // DECOM off (changes coordinate space)
        h.feed("\x1b[u");            // DECRC — should bring DECOM back on
        h.feed("\x1b[1;1H");         // CUP 1;1
        // If DECOM is restored to on, cursor lands at (4, 0).
        // If it stayed off (pre-fix), cursor lands at (0, 0).
        expectPos("I10", h, 4, 0);
    }

    // ---------- I11: ESC 7 / ESC 8 form has the same restore semantics ----------
    {
        Harness h;
        h.feed("\x1b[5;11r");
        h.feed("\x1b[?6h");
        h.feed("\x1b""7");           // ESC 7 (DECSC)
        h.feed("\x1b[?6l");
        h.feed("\x1b""8");           // ESC 8 (DECRC)
        h.feed("\x1b[1;1H");
        expectPos("I11", h, 4, 0);
    }

    // ---------- I12: DECSC saves DECAWM and DECRC restores it ----------
    {
        Harness h;
        h.feed("\x1b[?7l");          // DECAWM off
        h.feed("\x1b[s");            // DECSC
        h.feed("\x1b[?7h");          // DECAWM on
        h.feed("\x1b[u");            // DECRC — DECAWM should be off again
        // With autoWrap off, writing at the last column does not advance.
        h.feed("\x1b[1;80H");        // park at last column
        h.feed("X");                 // print at last column
        // Pre-write cursor was col 79; with delayed-wrap-off, after the
        // print the cursor still sits at col 79 (the just-written cell).
        // With autoWrap on, it would be col 79 with wrapNext set, then
        // a second print would bump us to (1, 0). Easiest to test: write
        // a SECOND char and check we did NOT wrap.
        h.feed("Y");
        if (h.row() != 0) {
            char buf[120];
            std::snprintf(buf, sizeof(buf),
                          "DECAWM not restored: cursor wrapped to row %d",
                          h.row());
            fail("I12", buf);
        }
    }

    // ---------- Source-grep checks: lock the implementation shape ----------
    const std::string src = readFile(SRC_TERMINALGRID_CPP_PATH);
    if (src.empty()) {
        fail("SRC", "could not read terminalgrid.cpp at "
                    SRC_TERMINALGRID_CPP_PATH);
    } else {
        // I-SRC1: CUP path adds m_scrollTop and clamps to scrollBottom
        // when m_originMode is set.
        const auto cupAt = src.find("case 'H':");
        const auto clampedAt = src.find("std::clamp(row, m_scrollTop, m_scrollBottom)");
        if (cupAt == std::string::npos || clampedAt == std::string::npos
            || clampedAt < cupAt || clampedAt - cupAt > 600) {
            fail("I-SRC1",
                 "CUP path doesn't have the origin-mode translate "
                 "(expected std::clamp(row, m_scrollTop, m_scrollBottom) "
                 "near case 'H')");
        }

        // I-SRC2: saveCursor stores m_originMode and m_autoWrap.
        const auto saveAt = src.find("void TerminalGrid::saveCursor()");
        if (saveAt == std::string::npos) {
            fail("I-SRC2", "saveCursor definition not found");
        } else {
            const std::string saveBody = src.substr(saveAt, 400);
            if (saveBody.find("m_savedOriginMode = m_originMode") == std::string::npos)
                fail("I-SRC2",
                     "saveCursor doesn't save m_originMode");
            if (saveBody.find("m_savedAutoWrap = m_autoWrap") == std::string::npos)
                fail("I-SRC2",
                     "saveCursor doesn't save m_autoWrap");
        }

        // I-SRC3: restoreCursor restores both flags.
        const auto restoreAt = src.find("void TerminalGrid::restoreCursor()");
        if (restoreAt == std::string::npos) {
            fail("I-SRC3", "restoreCursor definition not found");
        } else {
            const std::string body = src.substr(restoreAt, 500);
            if (body.find("m_originMode = m_savedOriginMode") == std::string::npos)
                fail("I-SRC3",
                     "restoreCursor doesn't restore m_originMode");
            if (body.find("m_autoWrap = m_savedAutoWrap") == std::string::npos)
                fail("I-SRC3",
                     "restoreCursor doesn't restore m_autoWrap");
        }

        // I-SRC4: setScrollRegion homes cursor in origin-mode aware fashion.
        const auto stbmAt = src.find("void TerminalGrid::setScrollRegion(");
        if (stbmAt == std::string::npos) {
            fail("I-SRC4", "setScrollRegion definition not found");
        } else {
            const std::string body = src.substr(stbmAt, 500);
            if (body.find("m_originMode ? m_scrollTop : 0") == std::string::npos)
                fail("I-SRC4",
                     "setScrollRegion doesn't home origin-mode-aware "
                     "(expected m_originMode ? m_scrollTop : 0)");
        }
    }

    return g_failures == 0 ? 0 : 1;
}
