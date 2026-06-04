// Feature-conformance test for spec.md (ANTS-1999) — m_scrollback and
// m_scrollbackHyperlinks must stay in lockstep across the session-restore
// path (pushScrollbackLine) and setMaxScrollback eviction, so OSC 8 spans
// map to the correct scrollback row after a restore.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <string>

namespace {

struct Harness {
    TerminalGrid grid;
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    explicit Harness(int rows, int cols) : grid(rows, cols) {}
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

// Scrollback index whose cells spell `label` (the link line), or -1.
int findLineByText(const TerminalGrid &g, const std::string &label) {
    for (int i = 0; i < g.scrollbackSize(); ++i) {
        const auto &cells = g.scrollbackLine(i);
        std::string s;
        for (const auto &c : cells)
            if (c.codepoint < 128) s += static_cast<char>(c.codepoint);
        if (s.find(label) != std::string::npos) return i;
    }
    return -1;
}

bool idxHasUri(const TerminalGrid &g, int idx, const std::string &url) {
    for (const auto &sp : g.scrollbackHyperlinks(idx))
        if (sp.uri == url) return true;
    return false;
}

}  // namespace

// INV-1 — restore lines then a real OSC 8 line scrolled into scrollback:
// the span lives at the same index as the link's text.
TEST(ScrollbackHyperlinkRestoreSync, Inv1RestoreThenLinkAligns) {
    Harness h(3, 20);
    // Simulate session restore: push 4 lines straight into scrollback.
    for (int i = 0; i < 4; ++i)
        h.grid.pushScrollbackLine(TermLine{});

    const std::string url = "https://restore.example/";
    h.feed(cup(0, 0));
    h.feed(osc8Link(url, "linkA"));
    // Scroll the link row off the top into scrollback.
    h.feed(cup(2, 0));
    h.feed("\n\n\n\n");

    const int idx = findLineByText(h.grid, "linkA");
    ASSERT_GE(idx, 0) << "link line must be in scrollback";
    EXPECT_TRUE(idxHasUri(h.grid, idx, url))
        << "span must map to the link's own row (idx=" << idx << ")";
}

// INV-2 — setMaxScrollback eviction keeps a surviving link's span aligned.
TEST(ScrollbackHyperlinkRestoreSync, Inv2SetMaxScrollbackKeepsAlignment) {
    Harness h(3, 20);
    // Fill scrollback past the eviction floor (1000) with restore lines,
    // THEN add the link last so it survives a front-trim.
    for (int i = 0; i < 1200; ++i)
        h.grid.pushScrollbackLine(TermLine{});

    const std::string url = "https://trim.example/";
    h.feed(cup(0, 0));
    h.feed(osc8Link(url, "linkB"));
    h.feed(cup(2, 0));
    h.feed("\n\n\n\n");

    // Now shrink the cap: setMaxScrollback pops front lines from both deques.
    h.grid.setMaxScrollback(1000);
    ASSERT_LE(h.grid.scrollbackSize(), 1000);

    const int idx = findLineByText(h.grid, "linkB");
    ASSERT_GE(idx, 0) << "link line must survive the front-trim";
    EXPECT_TRUE(idxHasUri(h.grid, idx, url))
        << "span must stay aligned after setMaxScrollback (idx=" << idx << ")";
}
