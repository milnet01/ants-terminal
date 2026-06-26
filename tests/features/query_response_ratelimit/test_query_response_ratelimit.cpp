// ANTS-2192 — query-response amplification cap. A program streaming a
// query (`\e[6n` in a tight loop) must NOT force one unbounded PTY write
// per request. Every DA/CPR/DSR/DECRQSS/colour/Kitty reply funnels
// through TerminalGrid::sendQueryResponse(), which applies a rolling
// 1 s cap (QUERY_RESP_MAX_PER_SEC = 256). See spec.md.
//
// The test drives the public VtParser → processAction path and counts
// replies via the response callback. It uses only public API, so it
// compiles (and goes red) against pre-fix code.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

struct Probe {
    TerminalGrid grid;
    VtParser     parser;
    int          responses = 0;

    Probe()
        : grid(kRows, kCols),
          parser([this](const VtAction &a) { grid.processAction(a); }) {
        grid.setResponseCallback(
            [this](const std::string &) { ++responses; });
    }

    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

}  // namespace

// INV-1 — a single query still gets its reply (the synchronous
// query/wait path is never throttled).
TEST(QueryResponseRateLimit, SingleQueryAnswered) {
    Probe p;
    p.feed("\x1B[6n");  // CPR
    EXPECT_EQ(p.responses, 1)
        << "a lone cursor-position query must be answered exactly once";
}

// INV-2 — a flood is capped near QUERY_RESP_MAX_PER_SEC, not one reply
// per query. This is the red→green discriminator: pre-fix all kFlood
// queries echoed a response.
TEST(QueryResponseRateLimit, FloodIsCapped) {
    Probe p;
    constexpr int kFlood = 4000;
    for (int i = 0; i < kFlood; ++i)
        p.feed("\x1B[6n");  // CPR flood, all within one wall-clock second

    // The cap is 256/sec. The whole loop runs in well under a second, so
    // at most one window-boundary crossing is possible → at most ~512
    // replies. We assert a generous 1000 ceiling: robust to a boundary
    // crossing yet decisively below the pre-fix kFlood=4000. Some replies
    // must still get through (the cap throttles, it doesn't mute).
    EXPECT_GT(p.responses, 0) << "the cap must not mute all replies";
    EXPECT_LT(p.responses, 1000)
        << "a query flood must be throttled, not echoed one-for-one "
           "(pre-fix this was " << kFlood << ")";
}

// INV-3 — a delivered reply still carries the correct content (the cap
// drops whole responses; it never mangles a delivered one).
TEST(QueryResponseRateLimit, DeliveredReplyContentIntact) {
    TerminalGrid grid(kRows, kCols);
    std::string captured;
    grid.setResponseCallback(
        [&captured](const std::string &s) { captured.append(s); });
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    parser.feed("\x1B[6n", 4);
    // Cursor at home (row 1, col 1) → CSI 1;1 R.
    EXPECT_EQ(captured, std::string("\x1B[1;1R"));
}
