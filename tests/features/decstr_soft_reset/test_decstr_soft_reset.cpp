// Feature-conformance test for spec.md (ANTS-1196) — DECSTR
// `CSI ! p` (Soft Terminal Reset) per xterm ctlseqs / VT220 ref.
//
// Drives the parser end-to-end with the literal byte sequence
// `\x1b[!p` so the test exercises intermediate-byte collection in
// vtparser → handleCsi finalChar 'p' with intermediate "!".
//
// ANTS-1217 Phase 2: migrated to GoogleTest TEST() blocks under
// Suite DecstrSoftReset.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>
#include <string>

namespace {

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
TEST(DecstrSoftReset, OriginMode) {
    Harness h;
    h.feed("\x1b[?6h");  // DECOM on
    ASSERT_TRUE(h.grid.originMode())
        << "setup: originMode not set by CSI ?6 h";
    h.decstr();
    EXPECT_FALSE(h.grid.originMode())
        << "originMode=true after DECSTR, expected false";
}

// INV-2 — DECSTR resets auto-wrap to on.
TEST(DecstrSoftReset, AutoWrap) {
    Harness h;
    h.feed("\x1b[?7l");  // DECAWM off
    ASSERT_FALSE(h.grid.autoWrap())
        << "setup: autoWrap still on after CSI ?7 l";
    h.decstr();
    EXPECT_TRUE(h.grid.autoWrap())
        << "autoWrap=false after DECSTR, expected true";
}

// INV-3 — DECSTR resets scroll region to full screen.
TEST(DecstrSoftReset, ScrollRegion) {
    Harness h(24, 80);
    h.feed("\x1b[5;15r");  // DECSTBM 5;15 → 0-indexed 4..14
    ASSERT_EQ(h.grid.scrollTop(), 4);
    ASSERT_EQ(h.grid.scrollBottom(), 14);
    h.decstr();
    EXPECT_EQ(h.grid.scrollTop(), 0);
    EXPECT_EQ(h.grid.scrollBottom(), 23);
}

// INV-4 — DECSTR resets SGR.
TEST(DecstrSoftReset, SGR) {
    Harness h;
    h.feed("\x1b[1;31m");  // bold red fg
    h.decstr();
    h.feed("X");  // print one cell with current attrs
    const Cell &c = h.grid.cellAt(0, 0);
    ASSERT_EQ(c.codepoint, 'X');
    EXPECT_FALSE(c.attrs.bold)
        << "cell still bold after DECSTR + print";
}

// INV-5 — DECSTR makes cursor visible.
TEST(DecstrSoftReset, CursorVisible) {
    Harness h;
    h.feed("\x1b[?25l");  // hide cursor (DECTCEM off)
    ASSERT_FALSE(h.grid.cursorVisible());
    h.decstr();
    EXPECT_TRUE(h.grid.cursorVisible())
        << "cursor still hidden after DECSTR, expected visible";
}

// INV-6 — DECSTR does NOT wipe the screen.
TEST(DecstrSoftReset, DoesNotWipeScreen) {
    Harness h;
    h.feed("hello");
    h.decstr();
    EXPECT_EQ(h.grid.cellAt(0, 0).codepoint, 'h')
        << "DECSTR is soft — must NOT wipe the buffer";
}

// INV-7 — DECSTR does NOT clear integration callbacks.
//
// Witness: install a response callback. After DECSTR, trigger CPR
// (`CSI 6 n` — cursor position report). If the callback survived
// the soft reset, we'll see the response captured here.
TEST(DecstrSoftReset, PreservesResponseCallback) {
    Harness h;
    std::string captured;
    h.grid.setResponseCallback(
        [&captured](const std::string &s) { captured += s; });
    h.decstr();
    captured.clear();
    h.feed("\x1b[6n");  // CPR — should fire the callback if alive
    ASSERT_FALSE(captured.empty())
        << "response callback dropped by DECSTR — CPR produced no output";
    // ANTS-2067 — require a CPR-shaped report (CSI … R), not merely a CSI
    // introducer. The old `|| find("\x1b[")` fallback accepted ANY escape
    // sequence, so it could not actually catch a dropped/garbled CPR. After
    // a soft reset the cursor is homed, so the report is ESC[1;1R; accept a
    // differing coordinate only if it still terminates in the CPR 'R'.
    const bool exactHomeCpr = captured.find("\x1b[1;1R") != std::string::npos;
    const bool cprShaped =
        captured.rfind("\x1b[", 0) == 0 && !captured.empty() &&
        captured.back() == 'R';
    EXPECT_TRUE(exactHomeCpr || cprShaped)
        << "response captured but didn't look like a CPR report: '"
        << captured << "'";
}

// INV-8 — DECSTR does NOT clear scrollback.
TEST(DecstrSoftReset, PreservesScrollback) {
    Harness h(5, 10);  // tiny grid for fast scrollback churn
    for (int i = 0; i < 20; ++i) {
        h.feed("line\r\n");
    }
    int before = h.grid.scrollbackSize();
    ASSERT_GT(before, 0) << "setup: scrollback empty after 20-line push";
    h.decstr();
    EXPECT_EQ(h.grid.scrollbackSize(), before)
        << "scrollback changed after DECSTR (was soft, RIS would wipe)";
}
