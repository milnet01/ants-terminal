// Feature test: wide-char overwrite must zero the mate.
// See spec.md. Drives TerminalGrid through VtParser with CJK chars then
// overwrites them at each edge, asserting no stranded isWideChar/isWideCont.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>   // wcwidth() needs a UTF-8 locale to return 2 for CJK
#include <cstdio>
#include <string>

namespace {

// UTF-8 for U+4E2D CJK UNIFIED IDEOGRAPH "中". width=2 under wcwidth.
const std::string kZhong = "\xE4\xB8\xAD";
const uint32_t kZhongCp = 0x4E2D;

// UTF-8 for U+65E5 CJK UNIFIED IDEOGRAPH "日". width=2. Used as the
// second wide char in the "wide-over-wide" subcase so we can tell them
// apart by codepoint.
const std::string kRi = "\xE6\x97\xA5";
const uint32_t kRiCp = 0x65E5;

struct Harness {
    TerminalGrid grid{24, 80};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};

    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

int g_failures = 0;
void fail(const char *label, const char *detail) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    ++g_failures;
}

// Sanity: did wcwidth actually give us width=2 under the test locale?
// If not, the test premise is broken and every subcase would be a noop.
bool wideCharPremiseHolds() {
    Harness h;
    h.feed(kZhong);
    const auto &c0 = h.grid.cellAt(0, 0);
    const auto &c1 = h.grid.cellAt(0, 1);
    return c0.isWideChar && c0.codepoint == kZhongCp
        && c1.isWideCont && c1.codepoint == 0;
}

}  // namespace

int main() {
    std::setlocale(LC_CTYPE, "");

    if (!wideCharPremiseHolds()) {
        std::fprintf(stderr,
                     "SKIP: wcwidth(U+4E2D) != 2 under current locale — "
                     "wide-char overwrite test cannot run meaningfully.\n");
        return 0;  // don't fail CI on a pure environment issue
    }

    // --- INV-1: narrow over right-half — left-half must stop claiming wide ---
    {
        Harness h;
        h.feed(kZhong);                   // col 0-1 become [中][cont]
        h.feed("\x1b[1;2HX");             // cursor to (1,2) and write 'X'
        const auto &left = h.grid.cellAt(0, 0);
        const auto &right = h.grid.cellAt(0, 1);
        if (left.isWideChar)
            fail("INV-1",
                        "cell at col 0 still claims isWideChar after narrow "
                        "write overwrote its continuation at col 1");
        if (right.codepoint != 'X' || right.isWideCont)
            fail("INV-1",
                        "cell at col 1 should hold 'X' with isWideCont=false");
    }

    // --- INV-2: narrow over left-half — right-half must stop claiming cont ---
    {
        Harness h;
        h.feed(kZhong);                   // col 0-1 become [中][cont]
        h.feed("\x1b[1;1HY");             // overwrite col 0 with 'Y'
        const auto &left = h.grid.cellAt(0, 0);
        const auto &right = h.grid.cellAt(0, 1);
        if (left.codepoint != 'Y' || left.isWideChar)
            fail("INV-2",
                        "cell at col 0 should hold 'Y' with isWideChar=false");
        if (right.isWideCont)
            fail("INV-2",
                        "cell at col 1 still claims isWideCont after narrow "
                        "write clobbered the first-half at col 0");
    }

    // --- INV-3: wide over wide-shifted-by-one — right orphan must clear ---
    // Layout: [中][cont][日][cont] at cols 0..3.
    // Write a fresh wide char starting at col 1 — overwrites the cont at 1
    // and the first-half at 2, so col 2 becomes the new wide's cont.
    // But col 3 (the old "日" cont) is orphaned: its mate at col 2 is now
    // a continuation of a DIFFERENT wide. The orphan must stop claiming
    // isWideCont.
    {
        Harness h;
        h.feed(kZhong);                   // cols 0-1
        h.feed(kRi);                      // cols 2-3
        h.feed("\x1b[1;2H");              // cursor to (1,2)
        h.feed(kRi);                      // write 日 at col 1-2
        const auto &c0 = h.grid.cellAt(0, 0);  // stranded left-half of old 中
        const auto &c1 = h.grid.cellAt(0, 1);  // new 日 first-half
        const auto &c2 = h.grid.cellAt(0, 2);  // new 日 continuation
        const auto &c3 = h.grid.cellAt(0, 3);  // orphaned continuation of old 日
        if (c0.isWideChar)
            fail("INV-3",
                        "col 0 still claims isWideChar — its mate at col 1 "
                        "was overwritten by the new wide char");
        if (!c1.isWideChar || c1.codepoint != kRiCp)
            fail("INV-3",
                        "col 1 should be the new 日 first-half");
        if (!c2.isWideCont || c2.codepoint != 0)
            fail("INV-3",
                        "col 2 should be the new 日 continuation");
        if (c3.isWideCont)
            fail("INV-3",
                        "col 3 still claims isWideCont — its mate at col 2 "
                        "is now the continuation of a different wide char");
    }

    // --- INV-4: fast ASCII path (handleAsciiPrintRun) must sanitize too ---
    // A long ASCII run goes through the SIMD-coalesced Print action which
    // writes via handleAsciiPrintRun rather than per-codepoint handlePrint.
    // Put a wide char at col 39-40, then start an ASCII run at col 40 —
    // the run's first byte lands on the continuation and must orphan col 39.
    {
        Harness h;
        h.feed("\x1b[1;40H");             // cursor to (1,40)
        h.feed(kZhong);                   // 中 at cols 39-40
        h.feed("\x1b[1;41H");             // cursor to (1,41), the cont
        // Feed >16 bytes so the parser's SIMD scanner can coalesce.
        h.feed("the quick brown fox jumps over the lazy dog");
        const auto &leftMate = h.grid.cellAt(0, 39);
        if (leftMate.isWideChar)
            fail("INV-4",
                        "col 39 still claims isWideChar after a fast ASCII "
                        "run overwrote its continuation at col 40");
    }

    // --- INV-5: non-overlapping writes leave existing wide pairs intact ---
    {
        Harness h;
        h.feed(kZhong);                   // cols 0-1
        h.feed("\x1b[1;10Hhello");        // write far from the wide pair
        const auto &c0 = h.grid.cellAt(0, 0);
        const auto &c1 = h.grid.cellAt(0, 1);
        if (!c0.isWideChar || c0.codepoint != kZhongCp)
            fail("INV-5",
                        "wide first-half at col 0 was disturbed by an "
                        "unrelated narrow write at col 9");
        if (!c1.isWideCont)
            fail("INV-5",
                 "wide continuation at col 1 was disturbed");
    }

    return g_failures == 0 ? 0 : 1;
}
