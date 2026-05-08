// Feature-conformance test for spec.md (ANTS-1196) — DECSTR
// `CSI ! p` (Soft Terminal Reset) per xterm ctlseqs / VT220 ref.
//
// Drives the parser end-to-end with the literal byte sequence
// `\x1b[!p` so the test exercises intermediate-byte collection in
// vtparser → handleCsi finalChar 'p' with intermediate "!".
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
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
    Harness(int rows = 24, int cols = 80)
        : grid(rows, cols),
          parser([this](const VtAction &a) { grid.processAction(a); }) {}
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
    void decstr() { feed("\x1b[!p"); }
};

}  // namespace

// INV-1 — DECSTR resets origin mode.
static void testInv1_OriginMode() {
    Harness h;
    h.feed("\x1b[?6h");  // DECOM on
    EXPECT(h.grid.originMode() == true,
           "INV-1 setup: originMode not set by CSI ?6 h");
    h.decstr();
    EXPECT(h.grid.originMode() == false,
           "INV-1: originMode=true after DECSTR, expected false");
}

// INV-2 — DECSTR resets auto-wrap to on.
static void testInv2_AutoWrap() {
    Harness h;
    h.feed("\x1b[?7l");  // DECAWM off
    EXPECT(h.grid.autoWrap() == false,
           "INV-2 setup: autoWrap still on after CSI ?7 l");
    h.decstr();
    EXPECT(h.grid.autoWrap() == true,
           "INV-2: autoWrap=false after DECSTR, expected true");
}

// INV-3 — DECSTR resets scroll region to full screen.
static void testInv3_ScrollRegion() {
    Harness h(24, 80);
    h.feed("\x1b[5;15r");  // DECSTBM 5;15 → 0-indexed 4..14
    EXPECT(h.grid.scrollTop() == 4 && h.grid.scrollBottom() == 14,
           "INV-3 setup: top=%d bottom=%d, expected 4/14",
           h.grid.scrollTop(), h.grid.scrollBottom());
    h.decstr();
    EXPECT(h.grid.scrollTop() == 0,
           "INV-3: scrollTop=%d after DECSTR, expected 0",
           h.grid.scrollTop());
    EXPECT(h.grid.scrollBottom() == 23,
           "INV-3: scrollBottom=%d after DECSTR, expected 23",
           h.grid.scrollBottom());
}

// INV-4 — DECSTR resets SGR.
static void testInv4_SGR() {
    Harness h;
    h.feed("\x1b[1;31m");  // bold red fg
    h.decstr();
    h.feed("X");  // print one cell with current attrs
    const Cell &c = h.grid.cellAt(0, 0);
    EXPECT(c.codepoint == 'X',
           "INV-4 setup: cellAt(0,0)='%c', expected 'X'", c.codepoint);
    EXPECT(c.attrs.bold == false,
           "INV-4: cell still bold after DECSTR + print");
}

// INV-5 — DECSTR makes cursor visible.
static void testInv5_CursorVisible() {
    Harness h;
    h.feed("\x1b[?25l");  // hide cursor (DECTCEM off)
    EXPECT(h.grid.cursorVisible() == false,
           "INV-5 setup: cursor still visible after CSI ?25 l");
    h.decstr();
    EXPECT(h.grid.cursorVisible() == true,
           "INV-5: cursor still hidden after DECSTR, expected visible");
}

// INV-6 — DECSTR does NOT wipe the screen.
static void testInv6_DoesNotWipeScreen() {
    Harness h;
    h.feed("hello");
    h.decstr();
    const Cell &c = h.grid.cellAt(0, 0);
    EXPECT(c.codepoint == 'h',
           "INV-6: cellAt(0,0)='%c' after DECSTR, expected 'h' "
           "(DECSTR is soft — must NOT wipe the buffer)",
           c.codepoint);
}

// INV-7 — DECSTR does NOT clear integration callbacks.
//
// Witness: install a response callback. After DECSTR, trigger CPR
// (`CSI 6 n` — cursor position report). If the callback survived
// the soft reset, we'll see the response captured here.
static void testInv7_PreservesResponseCallback() {
    Harness h;
    std::string captured;
    h.grid.setResponseCallback(
        [&captured](const std::string &s) { captured += s; });
    h.decstr();
    captured.clear();
    h.feed("\x1b[6n");  // CPR — should fire the callback if alive
    EXPECT(!captured.empty(),
           "INV-7: response callback dropped by DECSTR — CPR produced "
           "no output");
    EXPECT(captured.find("\x1b[1;1R") != std::string::npos ||
           captured.find("\x1b[") != std::string::npos,
           "INV-7: response captured but didn't look like CPR (got '%s')",
           captured.c_str());
}

// INV-8 — DECSTR does NOT clear scrollback.
//
// Push more lines than the screen holds, verify scrollback grew,
// send DECSTR, verify scrollback survived.
static void testInv8_PreservesScrollback() {
    Harness h(5, 10);  // tiny grid for fast scrollback churn
    for (int i = 0; i < 20; ++i) {
        h.feed("line\r\n");
    }
    int before = h.grid.scrollbackSize();
    EXPECT(before > 0,
           "INV-8 setup: scrollback empty after 20-line push, "
           "expected > 0");
    h.decstr();
    int after = h.grid.scrollbackSize();
    EXPECT(after == before,
           "INV-8: scrollback shrunk after DECSTR (%d → %d), "
           "expected unchanged (DECSTR is soft, RIS would wipe)",
           before, after);
}

int main() {
    testInv1_OriginMode();
    testInv2_AutoWrap();
    testInv3_ScrollRegion();
    testInv4_SGR();
    testInv5_CursorVisible();
    testInv6_DoesNotWipeScreen();
    testInv7_PreservesResponseCallback();
    testInv8_PreservesScrollback();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall 8 invariants hold\n");
    return 0;
}
