// Feature-conformance test for spec.md — asserts combining-char side
// tables survive TerminalGrid::resize for both main screen and alt
// screen, and that columns dropped by shrinking are cleanly evicted.
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <clocale>    // setlocale — wcwidth() needs a UTF-8 locale to
                      // classify combining codepoints correctly; under the
                      // default POSIX "C" locale wcwidth(U+0301) returns -1
                      // and the parser treats the combiner as a normal
                      // printable, corrupting the test premise. The main
                      // app binary inherits the user's locale via QCore-
                      // Application; test executables must opt in manually.
#include <cstdio>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

// UTF-8 for U+0065 LATIN SMALL LETTER E + U+0301 COMBINING ACUTE ACCENT.
// NFD canonical composition of 'é'. The parser's UTF-8 decoder emits
// the combining codepoint as an attachment to the preceding base cell.
const std::string kEAcuteNfd = "e\xCC\x81";

struct Harness {
    TerminalGrid grid{kRows, kCols};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};

    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

bool hasCombiningAt(const std::unordered_map<int, std::vector<uint32_t>> &m,
                    int col, uint32_t cp) {
    auto it = m.find(col);
    if (it == m.end()) return false;
    for (uint32_t c : it->second) if (c == cp) return true;
    return false;
}

int fail(const char *label, const char *detail) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    return 1;
}

}  // namespace

int main() {
    std::setlocale(LC_CTYPE, "");  // adopt user locale for wcwidth()

    // --- I1: main-screen preservation across widening resize ---
    {
        Harness h;
        h.feed(kEAcuteNfd);  // writes to row 0, col 0
        if (!hasCombiningAt(h.grid.screenCombining(0), 0, 0x0301))
            return fail("I1 baseline",
                        "combining acute not recorded at (0,0) before resize");

        h.grid.resize(kRows, kCols + 20);
        if (!hasCombiningAt(h.grid.screenCombining(0), 0, 0x0301))
            return fail("I1 after widening resize",
                        "combining acute lost after resize(rows, cols+20)");
    }

    // --- I2: alt-screen preservation across widening resize ---
    {
        Harness h;
        h.feed("\033[?1049h");  // enter alt screen
        h.feed(kEAcuteNfd);
        if (!h.grid.altScreenActive())
            return fail("I2 setup", "alt screen did not activate on 1049h");
        if (!hasCombiningAt(h.grid.screenCombining(0), 0, 0x0301))
            return fail("I2 baseline",
                        "combining acute not recorded on alt screen at (0,0)");

        h.grid.resize(kRows, kCols + 20);
        if (!hasCombiningAt(h.grid.screenCombining(0), 0, 0x0301))
            return fail("I2 after widening resize",
                        "combining acute lost on alt-screen after resize");
    }

    // --- I3: shrink drops out-of-range columns, keeps in-range ones ---
    {
        Harness h;
        // Place combining at column 10 (safe distance from both 0 and 79).
        h.feed("          ");           // 10 spaces to position cursor
        h.feed(kEAcuteNfd);             // writes base+combining at col 10
        if (!hasCombiningAt(h.grid.screenCombining(0), 10, 0x0301))
            return fail("I3 baseline",
                        "combining not recorded at col 10 before shrink");

        // Shrink to 15 cols — col 10 is still in range.
        h.grid.resize(kRows, 15);
        if (!hasCombiningAt(h.grid.screenCombining(0), 10, 0x0301))
            return fail("I3 after in-range shrink",
                        "combining lost on shrink that kept col 10");

        // Shrink to 2 cols — col 10 is now out of range.
        h.grid.resize(kRows, 2);
        const auto &m = h.grid.screenCombining(0);
        if (m.find(10) != m.end())
            return fail("I3 after out-of-range shrink",
                        "combining at col 10 should have been evicted");
        for (const auto &kv : m) {
            if (kv.first >= 2) {
                std::fprintf(stderr, "FAIL [I3 residue]: combining map still "
                            "contains col=%d after resize to cols=2\n", kv.first);
                return 1;
            }
        }
    }

    std::printf("combining_on_resize: 3 invariants held across 3 scenarios\n");
    return 0;
}
