// Feature-conformance test for spec.md (ANTS-1197) — OSC 9 vs
// OSC 9;4 disambiguator. Indie-review #3 finding.
//
// Drives the parser end-to-end with the literal byte sequences so
// the test exercises OSC payload assembly + handleOsc dispatch.
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

struct ProgressCapture {
    bool fired = false;
    ProgressState state = ProgressState::None;
    int percent = -1;
};

struct NotifyCapture {
    bool fired = false;
    QString title;
    QString body;
};

struct Harness {
    TerminalGrid grid;
    VtParser parser;
    ProgressCapture prog;
    NotifyCapture notif;

    Harness() : grid(24, 80),
                parser([this](const VtAction &a) { grid.processAction(a); }) {
        grid.setProgressCallback([this](ProgressState s, int p) {
            prog.fired = true;
            prog.state = s;
            prog.percent = p;
        });
        grid.setNotifyCallback([this](const QString &t, const QString &b) {
            notif.fired = true;
            notif.title = t;
            notif.body = b;
        });
    }
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

}  // namespace

// INV-1 — OSC 9 ; 4 ST (no body, length 3) → progress state 0.
static void testInv1_MinimalClearProgress() {
    Harness h;
    h.feed("\x1b]9;4\x1b\\");
    EXPECT(h.prog.fired,
           "INV-1: progress callback NOT fired for `9;4` minimal form "
           "(this was the user-reported bug — `9;4` was misclassified "
           "as notification with body \"4\")");
    EXPECT(h.prog.state == ProgressState::None,
           "INV-1: state=%d, expected None(0)", static_cast<int>(h.prog.state));
    EXPECT(h.prog.percent == 0,
           "INV-1: percent=%d, expected 0", h.prog.percent);
    EXPECT(!h.notif.fired,
           "INV-1: notification callback fired (body=`%s`) — should NOT "
           "fire for `9;4`",
           h.notif.body.toUtf8().constData());
}

// INV-2 — OSC 9 ; 4 ; 0 ST → progress state 0 (explicit form).
static void testInv2_ExplicitClearProgress() {
    Harness h;
    h.feed("\x1b]9;4;0\x1b\\");
    EXPECT(h.prog.fired, "INV-2: progress not fired for `9;4;0`");
    EXPECT(h.prog.state == ProgressState::None,
           "INV-2: state=%d, expected None(0)", static_cast<int>(h.prog.state));
    EXPECT(h.prog.percent == 0, "INV-2: percent=%d, expected 0", h.prog.percent);
    EXPECT(!h.notif.fired, "INV-2: notification fired");
}

// INV-3 — OSC 9 ; 4 ; 1 ; 42 ST → progress state 1, percent 42.
static void testInv3_FullProgress() {
    Harness h;
    h.feed("\x1b]9;4;1;42\x1b\\");
    EXPECT(h.prog.fired, "INV-3: progress not fired for `9;4;1;42`");
    EXPECT(h.prog.state == ProgressState::Normal,
           "INV-3: state=%d, expected Normal(1)",
           static_cast<int>(h.prog.state));
    EXPECT(h.prog.percent == 42,
           "INV-3: percent=%d, expected 42", h.prog.percent);
}

// INV-4 — OSC 9 ; 42 ST → notification body "42", NOT progress.
static void testInv4_TwoDigitNotification() {
    Harness h;
    h.feed("\x1b]9;42\x1b\\");
    EXPECT(h.notif.fired, "INV-4: notification not fired for `9;42`");
    EXPECT(h.notif.body == "42",
           "INV-4: notification body=`%s`, expected `42`",
           h.notif.body.toUtf8().constData());
    EXPECT(!h.prog.fired,
           "INV-4: progress fired (state=%d percent=%d) — should be "
           "notification body \"42\"",
           static_cast<int>(h.prog.state), h.prog.percent);
}

// INV-5 — OSC 9 ; Hello ST → notification body "Hello".
static void testInv5_StringNotification() {
    Harness h;
    h.feed("\x1b]9;Hello\x1b\\");
    EXPECT(h.notif.fired, "INV-5: notification not fired");
    EXPECT(h.notif.body == "Hello",
           "INV-5: body=`%s`, expected `Hello`",
           h.notif.body.toUtf8().constData());
    EXPECT(!h.prog.fired, "INV-5: progress fired");
}

// INV-6 — OSC 9 ; 4Hello ST → notification body "4Hello".
//
// Disambiguator rule: byte after `4` must be `;` or end-of-payload
// for progress. `4Hello` starts with `4` but the next byte is `H`,
// so this is a notification body.
static void testInv6_FourPrefixedNotification() {
    Harness h;
    h.feed("\x1b]9;4Hello\x1b\\");
    EXPECT(h.notif.fired, "INV-6: notification not fired for `9;4Hello`");
    EXPECT(h.notif.body == "4Hello",
           "INV-6: body=`%s`, expected `4Hello`",
           h.notif.body.toUtf8().constData());
    EXPECT(!h.prog.fired,
           "INV-6: progress fired (state=%d percent=%d) — should be "
           "notification body \"4Hello\"",
           static_cast<int>(h.prog.state), h.prog.percent);
}

int main() {
    testInv1_MinimalClearProgress();
    testInv2_ExplicitClearProgress();
    testInv3_FullProgress();
    testInv4_TwoDigitNotification();
    testInv5_StringNotification();
    testInv6_FourPrefixedNotification();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall 6 invariants hold\n");
    return 0;
}
