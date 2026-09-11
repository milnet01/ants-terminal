// Feature-conformance test for spec.md (ANTS-5033) — desktop-notification
// rate quota, shared across OSC 9 and OSC 777.
//
// Why this exists: TerminalGrid::handleOsc forwarded every OSC 9 body and
// every OSC 777 notify;title;body sequence straight to m_notifyCallback
// with no rate limit, while every sibling that reaches outside the
// terminal (OSC 52, OSC 133, query responses) already carries one. A
// flood spawns one notify-send process per sequence from the GUI thread.
//
// Drives the parser end-to-end with literal escape bytes so the test
// exercises OSC payload assembly + handleOsc dispatch, same pattern as
// tests/features/osc9_progress_disambiguator.
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <string>

namespace {

#undef EXPECT
#define EXPECT(cond, ...) do {                                  \
    if (!(cond)) {                                              \
        char _ants_msg[512];                                    \
        std::snprintf(_ants_msg, sizeof(_ants_msg), __VA_ARGS__); \
        ADD_FAILURE_AT(__FILE__, __LINE__) << _ants_msg;        \
    }                                                           \
} while (0)

// Generous upper bound on the decided quota (10/min); INV-1..3 assert
// against this, never against the exact constant (spec.md § Out of
// scope) — a future retune of the number should not break this test.
constexpr int kQuotaUpperBound = 32;

struct NotifyCounter {
    int count = 0;
};

struct ProgressCapture {
    bool fired = false;
    ProgressState state = ProgressState::None;
    int percent = -1;
};

struct Harness {
    TerminalGrid grid;
    VtParser parser;
    NotifyCounter notif;
    ProgressCapture prog;

    Harness() : grid(24, 80),
                parser([this](const VtAction &a) { grid.processAction(a); }) {
        grid.setNotifyCallback([this](const QString &, const QString &) {
            notif.count += 1;
        });
        grid.setProgressCallback([this](ProgressState s, int p) {
            prog.fired = true;
            prog.state = s;
            prog.percent = p;
        });
    }
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

}  // namespace

// INV-1 — an OSC 9 flood on one grid delivers at least one and at most
// kQuotaUpperBound callbacks, never one per sequence fed.
TEST(OscNotifyQuota, Osc9FloodCapped) {
    Harness h;
    constexpr int kFed = 300;
    for (int i = 0; i < kFed; ++i) {
        h.feed("\x1b]9;flood-" + std::to_string(i) + "\x1b\\");
    }
    EXPECT(h.notif.count >= 1,
           "INV-1: no OSC 9 notification delivered at all (fed=%d, "
           "delivered=%d) — the legitimate single-notification case must "
           "still work",
           kFed, h.notif.count);
    EXPECT(h.notif.count <= kQuotaUpperBound,
           "INV-1: OSC 9 flood delivered %d callbacks for %d sequences "
           "fed, expected <= %d — quota did not cap the flood",
           h.notif.count, kFed, kQuotaUpperBound);
}

// INV-2 — an OSC 777 flood on a fresh grid is capped the same way.
TEST(OscNotifyQuota, Osc777FloodCapped) {
    Harness h;
    constexpr int kFed = 300;
    for (int i = 0; i < kFed; ++i) {
        std::string body = "\x1b]777;notify;title-" + std::to_string(i) +
                            ";body-" + std::to_string(i) + "\x1b\\";
        h.feed(body);
    }
    EXPECT(h.notif.count >= 1,
           "INV-2: no OSC 777 notification delivered at all (fed=%d, "
           "delivered=%d)",
           kFed, h.notif.count);
    EXPECT(h.notif.count <= kQuotaUpperBound,
           "INV-2: OSC 777 flood delivered %d callbacks for %d sequences "
           "fed, expected <= %d — quota did not cap the flood",
           h.notif.count, kFed, kQuotaUpperBound);
}

// INV-3 — the quota is SHARED between OSC 9 and OSC 777, not one cap
// each. Alternating both codes past 2x the upper bound must still land
// at or under the single shared bound; a per-code quota would let
// roughly twice as many through.
TEST(OscNotifyQuota, QuotaSharedAcrossOsc9AndOsc777) {
    // Baseline: how many one code's flood delivers on a fresh grid. The
    // comparison below is independent of the quota's exact value.
    Harness single;
    constexpr int kFedPerCode = 200;
    for (int i = 0; i < kFedPerCode; ++i) {
        single.feed("\x1b]9;one-" + std::to_string(i) + "\x1b\\");
    }

    Harness h;
    for (int i = 0; i < kFedPerCode; ++i) {
        h.feed("\x1b]9;alt-9-" + std::to_string(i) + "\x1b\\");
        std::string osc777 = "\x1b]777;notify;t-" + std::to_string(i) +
                              ";b-" + std::to_string(i) + "\x1b\\";
        h.feed(osc777);
    }
    EXPECT(h.notif.count >= 1,
           "INV-3: no notification delivered at all from the alternating "
           "flood (fed %d of each code, delivered=%d)",
           kFedPerCode, h.notif.count);
    EXPECT(h.notif.count <= single.notif.count,
           "INV-3: alternating OSC 9 / OSC 777 flood delivered %d callbacks, "
           "more than the %d a flood of OSC 9 alone delivers — each code has "
           "its own quota instead of one shared quota",
           h.notif.count, single.notif.count);
    EXPECT(h.notif.count <= kQuotaUpperBound,
           "INV-3: alternating flood delivered %d callbacks (fed %d of each "
           "code), expected <= %d",
           h.notif.count, kFedPerCode, kQuotaUpperBound);
}

// INV-5 — a full reset (RIS, ESC c) does not refill the budget: a flood
// that resets the terminal before every notification is capped too.
TEST(OscNotifyQuota, ResetDoesNotRefillQuota) {
    constexpr int kFed = 200;
    Harness single;
    for (int i = 0; i < kFed; ++i) {
        single.feed("\x1b]9;one-" + std::to_string(i) + "\x1b\\");
    }

    Harness h;
    for (int i = 0; i < kFed; ++i) {
        // "\x1b" "c" is ESC c; written apart so the hex escape stops at 1b.
        h.feed("\x1b" "c" "\x1b]9;reset-" + std::to_string(i) + "\x1b\\");
    }
    EXPECT(h.notif.count >= 1,
           "INV-5: no notification delivered at all from the reset flood "
           "(fed=%d, delivered=%d)", kFed, h.notif.count);
    EXPECT(h.notif.count <= single.notif.count,
           "INV-5: a flood that sends RIS before each notification delivered "
           "%d, more than the %d an uninterrupted flood delivers — the reset "
           "refilled the quota", h.notif.count, single.notif.count);
    EXPECT(h.notif.count <= kQuotaUpperBound,
           "INV-5: reset flood delivered %d callbacks for %d fed, expected "
           "<= %d", h.notif.count, kFed, kQuotaUpperBound);
}

// INV-4 — OSC 9;4 progress is a distinct branch and is never consumed by
// the notification quota, even after that quota is exhausted.
TEST(OscNotifyQuota, Osc94ProgressSurvivesNotifyFlood) {
    Harness h;
    constexpr int kFed = 300;
    for (int i = 0; i < kFed; ++i) {
        h.feed("\x1b]9;exhaust-" + std::to_string(i) + "\x1b\\");
    }

    h.feed("\x1b]9;4;1;42\x1b\\");
    EXPECT(h.prog.fired,
           "INV-4: OSC 9;4 progress callback did NOT fire after a "
           "notification flood exhausted the quota — progress must not "
           "share the notification quota");
    EXPECT(h.prog.state == ProgressState::Normal,
           "INV-4: progress state=%d, expected Normal(1)",
           static_cast<int>(h.prog.state));
    EXPECT(h.prog.percent == 42,
           "INV-4: progress percent=%d, expected 42", h.prog.percent);
}
