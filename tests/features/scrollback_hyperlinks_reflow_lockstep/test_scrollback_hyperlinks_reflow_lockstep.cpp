// Feature-conformance test for spec.md — ANTS-1333. Asserts the
// scrollback hyperlink deque stays length-locked to the scrollback
// row deque through TerminalGrid::resize() width-change reflow.
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

std::string osc8Link(const std::string &url, const std::string &label) {
    return "\033]8;;" + url + "\x07" + label + "\033]8;;\x07";
}

// Probe m_scrollbackHyperlinks' true length via reference identity.
// terminalgrid.cpp:225-230 returns a reference to a function-static
// empty vector for out-of-range idx; in-range probes return a
// reference into the deque. Same-address-as-fallback ⇒ out of range.
int hyperlinksDequeSize(const TerminalGrid &g) {
    const auto *fallback = &g.scrollbackHyperlinks(1 << 30);
    int lo = 0, hi = 1 << 20;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (&g.scrollbackHyperlinks(mid) == fallback) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

bool dequesLockstep(const TerminalGrid &g) {
    return hyperlinksDequeSize(g) == g.scrollbackSize();
}

void fail(const char *label, const char *detail){
    std::fprintf(stderr, "FAIL [%s]: %s\n", label, detail);
    ADD_FAILURE() << label << ": " << detail;
}

// Push N rows into scrollback. Each row gets a hyperlink at col 5,
// then we feed kRows LFs so the row scrolls off the top into
// scrollback. LF at the bottom of the scroll region triggers
// scrollUp(1), which pushes screen row 0 (with its committed
// hyperlinks) into m_scrollback + m_scrollbackHyperlinks.
void seedScrollback(Harness &h, int rows) {
    for (int i = 0; i < rows; ++i) {
        // Home + pad to col 5 + drop hyperlink wrapping "abcdefghij".
        h.feed("\033[H");
        h.feed("     ");
        h.feed(osc8Link("https://row" + std::to_string(i) + ".example/",
                        "abcdefghij"));  // 10 chars at cols 5..14
        // kRows LFs drive cursor past the bottom; the kRows-th LF
        // triggers scrollUp(1) pushing the row containing our
        // hyperlink (originally row 0) into scrollback.
        for (int j = 0; j < kRows; ++j) h.feed("\n");
    }
}

}  // namespace

TEST(ScrollbackHyperlinksReflowLockstep, Main) {
    std::setlocale(LC_CTYPE, "");

    // --- INV-1 (baseline) lockstep before any reflow ---
    {
        Harness h;
        seedScrollback(h, 5);
        if (h.grid.scrollbackSize() < 1)
            fail("baseline seed",
                        "scrollback empty after seeding 5 hyperlinks");
        // At least one seeded row must carry a hyperlink so later
        // assertions can distinguish "spans dropped by reflow" from
        // "seed never put spans in scrollback".
        bool anyBaseline = false;
        for (int i = 0; i < h.grid.scrollbackSize(); ++i) {
            if (!h.grid.scrollbackHyperlinks(i).empty()) {
                anyBaseline = true; break;
            }
        }
        if (!anyBaseline)
            fail("baseline span seed",
                        "seedScrollback didn't actually land any "
                        "hyperlinks in scrollback");
        if (!dequesLockstep(h.grid))
            fail("baseline lockstep",
                        "scrollback hyperlinks deque not locked to "
                        "scrollback length");
    }

    // --- INV-2 — in-place narrow (cols 80 → 60, links at 5..14 fit) ---
    {
        Harness h;
        seedScrollback(h, 5);
        const int sbBefore = h.grid.scrollbackSize();
        h.grid.resize(kRows, 60);
        if (!dequesLockstep(h.grid))
            fail("INV-2 narrow lockstep",
                        "deque drift after in-place narrow reflow");
        // Find any scrollback row whose hyperlinks vector is non-empty —
        // proves spans carried through. (Exact row count can differ
        // since some rows may have hit the slow path.)
        bool anySpan = false;
        for (int i = 0; i < h.grid.scrollbackSize(); ++i) {
            if (!h.grid.scrollbackHyperlinks(i).empty()) { anySpan = true; break; }
        }
        if (!anySpan)
            fail("INV-2 span survival",
                        "no scrollback row carries a hyperlink span "
                        "after in-place narrow; INV-2 requires the "
                        "fast-path rows preserve theirs");
        // Sanity: scrollback row count should be in the same ballpark
        // (no doubling or zeroing).
        if (h.grid.scrollbackSize() < sbBefore / 2 ||
            h.grid.scrollbackSize() > sbBefore * 4)
            fail("INV-2 row count",
                        "post-reflow scrollback size wildly different "
                        "— suggests reflow path took an unexpected branch");
    }

    // --- INV-2 — in-place grow (cols 80 → 100) ---
    {
        Harness h;
        seedScrollback(h, 5);
        h.grid.resize(kRows, 100);
        if (!dequesLockstep(h.grid))
            fail("INV-2 grow lockstep",
                        "deque drift after in-place grow reflow");
        bool anySpan = false;
        for (int i = 0; i < h.grid.scrollbackSize(); ++i) {
            if (!h.grid.scrollbackHyperlinks(i).empty()) { anySpan = true; break; }
        }
        if (!anySpan)
            fail("INV-2 grow span survival",
                        "no scrollback row carries a hyperlink span "
                        "after in-place grow");
    }

    // --- INV-3 — slow rewrap path (cols 80 → 10, content doesn't fit) ---
    {
        Harness h;
        seedScrollback(h, 5);
        h.grid.resize(kRows, 10);
        if (!dequesLockstep(h.grid))
            fail("INV-3 slow-path lockstep",
                        "deque drift after slow rewrap reflow");
        // Slow path drops spans (one empty vector per reflowed row).
        // We can't assert all rows are empty (cap-trim may have evicted
        // anyway), but lockstep is the load-bearing invariant.
    }

    // --- INV-4 — screen→scrollback overflow push (rows 24 → 12) ---
    {
        Harness h;
        // Drop a hyperlink on the screen so reducing rows must push
        // it through the overflow path.
        h.feed(cup(0, 0));
        h.feed(osc8Link("https://overflow.example/", "abcdefghij"));
        const int sbBefore = h.grid.scrollbackSize();
        h.grid.resize(12, kCols);
        if (!dequesLockstep(h.grid))
            fail("INV-4 overflow lockstep",
                        "deque drift after screen→scrollback overflow push");
        // If anything overflowed, scrollback grew.
        if (h.grid.scrollbackSize() <= sbBefore && sbBefore > 0)
            fail("INV-4 overflow ran",
                        "expected scrollback to grow on rows shrink");
    }

    // --- INV-5 (ANTS-2119 M1) — scrollbackPushed() advances when a width
    // reflow pushes overflow lines into scrollback. The header contract sells
    // it as the authoritative "lines ever pushed" count (get_scrollback's
    // since_cursor diff relies on it); only scrollUp used to increment it, so a
    // resize-reflow silently under-counted and since_cursor dropped those
    // lines. ---
    {
        Harness h;   // 24×80
        // One long logical line spanning ~10 screen rows; no scrollUp yet
        // (10 < 24 rows) so the pre-resize push count is a clean baseline.
        h.feed(cup(0, 0));
        h.feed(std::string(kCols * 10, 'x'));
        const uint64_t pushedBefore = h.grid.scrollbackPushed();
        // Narrow to 20 cols: the content rewraps to ~40 rows > 24, so ~16
        // overflow lines are pushed into scrollback via the reflow path.
        h.grid.resize(kRows, 20);
        if (h.grid.scrollbackPushed() <= pushedBefore)
            fail("INV-5 reflow counter",
                        "scrollbackPushed() must advance when a resize reflow "
                        "pushes overflow lines into scrollback (ANTS-2119 M1)");
    }

    // --- INV-6 (ANTS-2119 M1) — pushScrollbackLine() advances the counter.
    // SessionManager restore uses it to repopulate scrollback; without the
    // increment a restored session reports a stale "ever pushed" count. ---
    {
        Harness h;
        const uint64_t before = h.grid.scrollbackPushed();
        h.grid.pushScrollbackLine(TermLine{});
        h.grid.pushScrollbackLine(TermLine{});
        if (h.grid.scrollbackPushed() != before + 2)
            fail("INV-6 pushScrollbackLine counter",
                        "pushScrollbackLine() must advance scrollbackPushed() "
                        "once per pushed line (ANTS-2119 M1)");
    }

    std::printf("scrollback_hyperlinks_reflow_lockstep: "
                "7 invariants held\n");
}
