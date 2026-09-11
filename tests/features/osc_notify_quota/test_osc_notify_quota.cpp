// Feature-conformance test for spec.md (ANTS-5033 / ANTS-5122) —
// desktop-notification rate quota shared across OSC 9 and OSC 777, plus
// the OSC 52 clipboard and OSC 1337 SetUserVar write quotas.
//
// Why this exists: TerminalGrid::handleOsc forwarded every OSC 9 body and
// every OSC 777 notify;title;body sequence straight to m_notifyCallback
// with no rate limit, while every sibling that reaches outside the
// terminal (OSC 52, OSC 133, query responses) already carries one. A
// flood spawns one notify-send process per sequence from the GUI thread.
// ANTS-5122: RIS (ESC c) rebuilds the grid and refills every one of these
// quotas except the notification one (which ANTS-5033 already carries
// across) — a stream that resets before each write is never limited.
//
// Drives the parser end-to-end with literal escape bytes so the test
// exercises OSC payload assembly + handleOsc dispatch, same pattern as
// tests/features/osc9_progress_disambiguator.
//
// Exit 0 = invariants hold. Non-zero = regression.

#include "terminalgrid.h"
#include "vtparser.h"

#include <QByteArray>

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

// Generous upper bound on the decided notification quota (10/min);
// INV-1..3 assert against this, never against the exact constant
// (spec.md § Out of scope) — a future retune of the number should not
// break this test.
constexpr int kQuotaUpperBound = 32;

// ANTS-5122 — TerminalGrid::OSC52_MAX_WRITES_PER_MIN, OSC52_MAX_BYTES_PER_MIN
// and USERVAR_MAX_WRITES_PER_MIN (terminalgrid.h) are private, so a test
// binary can't read them directly. Mirrored here for SIZING the flood loops
// only — every INV-6..8 pass/fail assertion below compares a reset flood
// against a same-shape baseline flood measured in the same run, never
// against these numbers, so a future retune of any of them doesn't break
// this test (spec.md § Out of scope, same discipline as kQuotaUpperBound).
constexpr int kOsc52MaxWritesKnown = 32;
constexpr qint64 kOsc52MaxBytesKnown = 1 * 1024 * 1024;
constexpr int kUserVarMaxWritesKnown = 64;

struct NotifyCounter {
    int count = 0;
};

struct ProgressCapture {
    bool fired = false;
    ProgressState state = ProgressState::None;
    int percent = -1;
};

struct ClipboardCapture {
    int count = 0;
    qint64 bytes = 0;
};

struct UserVarCapture {
    int count = 0;
};

struct Harness {
    TerminalGrid grid;
    VtParser parser;
    NotifyCounter notif;
    ProgressCapture prog;
    ClipboardCapture clip;
    UserVarCapture uvar;

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
        grid.setClipboardCallback([this](const std::string &data, char) {
            clip.count += 1;
            clip.bytes += static_cast<qint64>(data.size());
        });
        grid.setUserVarCallback([this](const QString &, const QString &) {
            uvar.count += 1;
        });
    }
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

// Builds an OSC 52 clipboard-set sequence carrying `decodedSize` bytes of
// filler, base64-encoded (ESC ] 52 ; c ; <base64> ESC \).
std::string osc52Write(int decodedSize) {
    QByteArray raw(decodedSize, 'x');
    QByteArray b64 = raw.toBase64();
    return "\x1b]52;c;" + b64.toStdString() + "\x1b\\";
}

// Builds an OSC 1337 SetUserVar sequence with a fixed small value
// (ESC ] 1337 ; SetUserVar=<name>=<base64> ESC \).
std::string userVarWrite(const std::string &name) {
    QByteArray b64 = QByteArray("hi").toBase64();
    return "\x1b]1337;SetUserVar=" + name + "=" + b64.toStdString() + "\x1b\\";
}

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

// INV-6 — the OSC 52 per-minute WRITE-COUNT quota survives RIS. Each write
// is tiny so the byte quota never binds; only the write-count quota can
// explain any capping observed here.
TEST(OscNotifyQuota, Osc52WriteCountSurvivesReset) {
    // Comfortably exceeds the real per-minute write-count quota (known:
    // 32) so the baseline flood actually demonstrates capping.
    constexpr int kFed = 50;
    constexpr int kTinyBytes = 8;

    Harness baseline;
    for (int i = 0; i < kFed; ++i) {
        baseline.feed(osc52Write(kTinyBytes));
    }
    EXPECT(baseline.clip.count >= 1,
           "INV-6: no clipboard write delivered at all from the "
           "uninterrupted baseline (fed=%d, delivered=%d)",
           kFed, baseline.clip.count);
    EXPECT(baseline.clip.count < kFed,
           "INV-6: baseline delivered all %d writes fed — the write-count "
           "quota (known: %d/min) never bound, so this test can't tell a "
           "capped flood from an uncapped one",
           kFed, kOsc52MaxWritesKnown);

    Harness h;
    for (int i = 0; i < kFed; ++i) {
        // "\x1b" "c" is ESC c; written apart so the hex escape stops at 1b.
        h.feed("\x1b" "c" + osc52Write(kTinyBytes));
    }
    EXPECT(h.clip.count >= 1,
           "INV-6: no clipboard write delivered at all from the reset "
           "flood (fed=%d, delivered=%d)", kFed, h.clip.count);
    EXPECT(h.clip.count <= baseline.clip.count,
           "INV-6: a flood that sends RIS before each OSC 52 write "
           "delivered %d writes, more than the %d an uninterrupted flood "
           "delivers (known quota: %d/min) — the reset refilled the "
           "write-count budget",
           h.clip.count, baseline.clip.count, kOsc52MaxWritesKnown);
}

// INV-7 — the OSC 52 per-minute BYTE quota survives RIS. Each write is
// sized so the byte budget is exhausted well before the write-count
// quota could explain any capping (known counts: budget/chunk ≈ 26,
// write-count quota is 32) — decoded bytes stay modest (well under the
// per-write 1 MiB clipboard cap).
TEST(OscNotifyQuota, Osc52ByteBudgetSurvivesReset) {
    constexpr int kFed = 35;
    constexpr int kChunkBytes = 40000;  // ~39 KiB decoded per write

    Harness baseline;
    for (int i = 0; i < kFed; ++i) {
        baseline.feed(osc52Write(kChunkBytes));
    }
    EXPECT(baseline.clip.bytes >= 1,
           "INV-7: no clipboard bytes delivered at all from the "
           "uninterrupted baseline (fed=%d writes of %d bytes, "
           "delivered_bytes=%lld)",
           kFed, kChunkBytes, static_cast<long long>(baseline.clip.bytes));
    EXPECT(baseline.clip.bytes <= kOsc52MaxBytesKnown,
           "INV-7: baseline delivered %lld bytes, over the known %lld "
           "byte/min budget — the byte quota never bound, so this test "
           "can't tell a capped flood from an uncapped one",
           static_cast<long long>(baseline.clip.bytes),
           static_cast<long long>(kOsc52MaxBytesKnown));
    EXPECT(static_cast<qint64>(kFed) * kChunkBytes > kOsc52MaxBytesKnown,
           "INV-7: test setup bug — %d writes of %d bytes (%lld total) "
           "does not exceed the known %lld byte/min budget, so the flood "
           "was never large enough to trip the quota",
           kFed, kChunkBytes,
           static_cast<long long>(kFed) * kChunkBytes,
           static_cast<long long>(kOsc52MaxBytesKnown));

    Harness h;
    for (int i = 0; i < kFed; ++i) {
        h.feed("\x1b" "c" + osc52Write(kChunkBytes));
    }
    EXPECT(h.clip.bytes >= 1,
           "INV-7: no clipboard bytes delivered at all from the reset "
           "flood (fed=%d writes of %d bytes, delivered_bytes=%lld)",
           kFed, kChunkBytes, static_cast<long long>(h.clip.bytes));
    EXPECT(h.clip.bytes <= baseline.clip.bytes,
           "INV-7: a flood that sends RIS before each OSC 52 write "
           "delivered %lld bytes, more than the %lld an uninterrupted "
           "flood delivers (known budget: %lld bytes/min) — the reset "
           "refilled the byte budget",
           static_cast<long long>(h.clip.bytes),
           static_cast<long long>(baseline.clip.bytes),
           static_cast<long long>(kOsc52MaxBytesKnown));
}

// INV-8 — the OSC 1337 SetUserVar per-minute write quota survives RIS,
// mirroring OSC 52's write-count quota.
TEST(OscNotifyQuota, UserVarQuotaSurvivesReset) {
    // Comfortably exceeds the real per-minute quota (known: 64).
    constexpr int kFed = 90;

    Harness baseline;
    for (int i = 0; i < kFed; ++i) {
        baseline.feed(userVarWrite("foo"));
    }
    EXPECT(baseline.uvar.count >= 1,
           "INV-8: no SetUserVar delivered at all from the uninterrupted "
           "baseline (fed=%d, delivered=%d)", kFed, baseline.uvar.count);
    EXPECT(baseline.uvar.count < kFed,
           "INV-8: baseline delivered all %d SetUserVar writes fed — the "
           "quota (known: %d/min) never bound, so this test can't tell a "
           "capped flood from an uncapped one",
           kFed, kUserVarMaxWritesKnown);

    Harness h;
    for (int i = 0; i < kFed; ++i) {
        h.feed("\x1b" "c" + userVarWrite("foo"));
    }
    EXPECT(h.uvar.count >= 1,
           "INV-8: no SetUserVar delivered at all from the reset flood "
           "(fed=%d, delivered=%d)", kFed, h.uvar.count);
    EXPECT(h.uvar.count <= baseline.uvar.count,
           "INV-8: a flood that sends RIS before each SetUserVar write "
           "delivered %d, more than the %d an uninterrupted flood "
           "delivers (known quota: %d/min) — the reset refilled the "
           "budget",
           h.uvar.count, baseline.uvar.count, kUserVarMaxWritesKnown);
}

// INV-9 — guard. RIS still does its own job (grid contents/cursor cleared)
// while these quota counters are carried across it, and an uninterrupted
// flood is still capped exactly as it was before this fix (the write-count
// path, reusing the same assertion shape ris_preserves_callbacks already
// makes for grid-clear-on-RIS, per spec.md's instruction to reuse rather
// than invent one).
TEST(OscNotifyQuota, ResetStillClearsGridAndUnbrokenFloodStillLimited) {
    Harness h;
    // Seed a visible cell, same pattern as ris_preserves_callbacks.
    h.feed("\x1b[1;1HABC");
    ASSERT_EQ(h.grid.cellAt(0, 0).codepoint, static_cast<uint32_t>('A'));

    // A handful of clipboard writes, each preceded by RIS.
    constexpr int kFed = 5;
    for (int i = 0; i < kFed; ++i) {
        h.feed("\x1b" "c" + osc52Write(8));
    }

    EXPECT(h.grid.cursorRow() == 0,
           "INV-9: cursorRow=%d after a RIS-interleaved flood, expected 0 "
           "— RIS must still reset cursor position",
           h.grid.cursorRow());
    EXPECT(h.grid.cursorCol() == 0,
           "INV-9: cursorCol=%d after a RIS-interleaved flood, expected 0",
           h.grid.cursorCol());
    EXPECT(h.grid.cellAt(0, 0).codepoint != static_cast<uint32_t>('A'),
           "INV-9: cell(0,0) still holds the pre-RIS 'A' after a "
           "RIS-interleaved flood — RIS must still clear grid contents");

    // An uninterrupted flood (no resets) is capped exactly as it always
    // was: this fix only changes what RIS carries across, not the
    // no-reset quota path INV-1..3/6..8's baselines already exercise.
    Harness plain;
    constexpr int kPlainFed = 50;
    for (int i = 0; i < kPlainFed; ++i) {
        plain.feed(osc52Write(8));
    }
    EXPECT(plain.clip.count >= 1 && plain.clip.count < kPlainFed,
           "INV-9: an uninterrupted OSC 52 flood (no RIS) delivered %d of "
           "%d writes fed — the ordinary (no-reset) quota path must still "
           "cap it (known quota: %d/min), unchanged by carrying counters "
           "across RIS",
           plain.clip.count, kPlainFed, kOsc52MaxWritesKnown);
}
