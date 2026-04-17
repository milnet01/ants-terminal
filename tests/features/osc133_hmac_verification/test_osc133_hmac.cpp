// Feature-conformance test for spec.md — exercises the OSC 133 HMAC
// verifier against the canonical message forms across all four marker
// letters, both verifier-off and verifier-on configurations.
//
// Strategy: drive TerminalGrid via VtParser (same path as the PTY) with
// crafted OSC 133 byte streams, then inspect:
//   - grid.promptRegions().size() to verify markers were/weren't applied
//   - grid.osc133ForgeryCount() to verify the rejection counter
//
// HMAC computation uses QMessageAuthenticationCode in the test — same
// algorithm and canonical-message form as the production verifier. If
// either drifts, the test fails with a clear assertion site.
//
// Headless: no QApplication, no QGuiApplication. TerminalGrid + VtParser
// only.

#include "terminalgrid.h"
#include "vtparser.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QMessageAuthenticationCode>
#include <QString>

#include <cstdio>
#include <cstring>

namespace {
int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);  \
        ++failures;                                                          \
    }                                                                        \
} while (0)

#define CHECK_EQ(actual, expected, msg) do {                                 \
    if ((actual) != (expected)) {                                            \
        std::fprintf(stderr, "FAIL %s:%d  %s (actual=%lld expected=%lld)\n", \
                     __FILE__, __LINE__, msg,                                \
                     static_cast<long long>(actual),                         \
                     static_cast<long long>(expected));                      \
        ++failures;                                                          \
    }                                                                        \
} while (0)

// Compute HMAC-SHA256 hex over the canonical message. Mirrors
// TerminalGrid::verifyOsc133Hmac's expected message form. If marker is
// 'D' and extra is non-empty, the message includes the exit code.
QByteArray hmacHex(const QByteArray &key, char marker,
                    const QByteArray &promptId, const QByteArray &extra) {
    QByteArray msg;
    msg.append(marker);
    msg.append('|');
    msg.append(promptId);
    if (marker == 'D' && !extra.isEmpty()) {
        msg.append('|');
        msg.append(extra);
    }
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256, key);
    mac.addData(msg);
    return mac.result().toHex();
}

// Build an OSC 133 wire form. Use BEL (0x07) as the string terminator —
// the parser accepts both BEL and ESC-backslash; BEL is shorter.
QByteArray osc133Wire(char marker, const QByteArray &exitCode,
                      const QByteArray &aid, const QByteArray &ahmac) {
    QByteArray wire;
    wire.append("\033]133;");
    wire.append(marker);
    if (marker == 'D' && !exitCode.isEmpty()) {
        wire.append(';');
        wire.append(exitCode);
    }
    if (!aid.isEmpty()) {
        wire.append(";aid=");
        wire.append(aid);
    }
    if (!ahmac.isEmpty()) {
        wire.append(";ahmac=");
        wire.append(ahmac);
    }
    wire.append('\007');  // BEL
    return wire;
}

// VtParser uses a callback-on-action API (see vtparser.h: VtParser ctor
// takes a std::function<void(const VtAction&)>). We make a parser per
// feed call and route actions straight into the grid.
void feed(TerminalGrid &grid, const QByteArray &bytes) {
    VtParser parser([&grid](const VtAction &act) {
        grid.processAction(act);
    });
    parser.feed(bytes.constData(), bytes.size());
}

// --- Test 1 — verifier OFF: legacy OSC 133 forms still work. ---------
void testVerifierOffPermissive() {
    TerminalGrid grid(24, 80);
    

    CHECK(!grid.osc133HmacEnforced(),
          "verifier should be OFF when no key set (test precondition)");

    // Bare A — no aid, no ahmac. Should add a region.
    feed(grid, osc133Wire('A', "", "", ""));
    CHECK_EQ(grid.promptRegions().size(), 1u,
             "verifier OFF: bare OSC 133 A must create a prompt region");
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier OFF: forgery counter must NOT increment");

    // OSC 133 ; D ; 42 — legacy exit-code form. Should be accepted and
    // record exit 42 against the most recent region.
    feed(grid, osc133Wire('D', "42", "", ""));
    CHECK_EQ(grid.promptRegions().size(), 1u,
             "verifier OFF: legacy D should not add a region (closes existing)");
    CHECK_EQ(grid.promptRegions().back().exitCode, 42,
             "verifier OFF: legacy D must record the exit code");
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier OFF: forgery counter must remain 0");

    // Even with HMAC params present, verifier OFF accepts unconditionally.
    feed(grid, osc133Wire('A', "",
                                  QByteArray("p1"),
                                  QByteArray("ffffffff")));
    CHECK_EQ(grid.promptRegions().size(), 2u,
             "verifier OFF: marker with garbage HMAC must still be accepted");
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier OFF: forgery counter must remain 0");
}

// --- Test 2 — verifier ON: canonical valid markers accepted. ---------
void testVerifierOnAcceptsCorrect() {
    TerminalGrid grid(24, 80);
    
    const QByteArray key("test-key-32-bytes-of-arbitrary-data!");
    grid.setOsc133KeyForTest(key);
    CHECK(grid.osc133HmacEnforced(),
          "setOsc133KeyForTest should activate verifier");

    const QByteArray pid("p1-uuid-or-similar");
    feed(grid, osc133Wire('A', "", pid,
                                  hmacHex(key, 'A', pid, "")));
    CHECK_EQ(grid.promptRegions().size(), 1u,
             "verifier ON + correct A HMAC: region must be created");
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier ON + correct A HMAC: counter must stay 0");

    feed(grid, osc133Wire('B', "", pid,
                                  hmacHex(key, 'B', pid, "")));
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier ON + correct B HMAC: counter must stay 0");

    feed(grid, osc133Wire('C', "", pid,
                                  hmacHex(key, 'C', pid, "")));
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier ON + correct C HMAC: counter must stay 0");

    feed(grid, osc133Wire('D', "0", pid,
                                  hmacHex(key, 'D', pid, "0")));
    CHECK_EQ(grid.promptRegions().size(), 1u,
             "verifier ON + correct D HMAC: region count unchanged (closes existing)");
    CHECK_EQ(grid.promptRegions().back().exitCode, 0,
             "verifier ON + correct D HMAC: exit code recorded");
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier ON + correct D HMAC: counter must stay 0");

    // Uppercase-hex HMAC accepted (case-insensitive compare).
    QByteArray pid2("p2");
    QByteArray hmacUpper = hmacHex(key, 'A', pid2, "").toUpper();
    feed(grid, osc133Wire('A', "", pid2, hmacUpper));
    CHECK_EQ(grid.promptRegions().size(), 2u,
             "verifier ON + uppercase-hex HMAC: must still be accepted");
    CHECK_EQ(grid.osc133ForgeryCount(), 0,
             "verifier ON + uppercase-hex HMAC: counter must stay 0");
}

// --- Test 3 — verifier ON: forged / missing / mismatched markers dropped. ---
void testVerifierOnRejectsForged() {
    TerminalGrid grid(24, 80);
    
    const QByteArray key("another-test-key-32-byte-secret-1234");
    grid.setOsc133KeyForTest(key);

    // Marker with NO HMAC at all — verifier active means missing HMAC = forge.
    feed(grid, osc133Wire('A', "", "", ""));
    CHECK_EQ(grid.promptRegions().size(), 0u,
             "verifier ON + missing HMAC: must NOT create region");
    CHECK_EQ(grid.osc133ForgeryCount(), 1,
             "verifier ON + missing HMAC: counter must increment");

    // Marker with correct-shape but wrong-bytes HMAC.
    QByteArray pid("p1");
    QByteArray bogus(64, 'a');  // 64 hex chars, all 'a' — won't match
    feed(grid, osc133Wire('A', "", pid, bogus));
    CHECK_EQ(grid.promptRegions().size(), 0u,
             "verifier ON + wrong HMAC: must NOT create region");
    CHECK_EQ(grid.osc133ForgeryCount(), 2,
             "verifier ON + wrong HMAC: counter must increment");

    // promptId binding: HMAC computed for one promptId must not verify
    // for a different promptId on the wire.
    QByteArray hmacForP1 = hmacHex(key, 'A', "p1", "");
    feed(grid, osc133Wire('A', "", "p2", hmacForP1));
    CHECK_EQ(grid.promptRegions().size(), 0u,
             "verifier ON + HMAC bound to different promptId: must REJECT");
    CHECK_EQ(grid.osc133ForgeryCount(), 3,
             "verifier ON + promptId mismatch: counter must increment");

    // Exit-code binding: a D HMAC computed for exit code 0 must NOT
    // verify when the wire form claims exit code 1.
    QByteArray hmacForExit0 = hmacHex(key, 'D', pid, "0");
    feed(grid, osc133Wire('D', "1", pid, hmacForExit0));
    CHECK_EQ(grid.osc133ForgeryCount(), 4,
             "verifier ON + exit-code mismatch on D: counter must increment");
    // No region was opened (no valid A landed), so we don't assert exitCode here.

    // Now establish a real region with correct A, then attempt a forged D.
    QByteArray pid3("p3");
    feed(grid, osc133Wire('A', "", pid3, hmacHex(key, 'A', pid3, "")));
    CHECK_EQ(grid.promptRegions().size(), 1u,
             "valid A after forgeries: must create region");
    int countBeforeForgedD = grid.osc133ForgeryCount();

    feed(grid, osc133Wire('D', "0", pid3, bogus));
    CHECK_EQ(grid.osc133ForgeryCount(), countBeforeForgedD + 1,
             "forged D after valid A: counter must increment");
    CHECK_EQ(grid.promptRegions().back().exitCode, 0,
             "forged D must NOT update exit code (default 0 from struct init)");
    CHECK_EQ(grid.promptRegions().back().commandEndMs, qint64(0),
             "forged D must NOT mark commandEnd (region remains open)");
}

// --- Test 4 — production binding: ANTS_OSC133_KEY is the only env knob. ---
void testEnvKnobIsTheOnlyKnob() {
    QFile f(QStringLiteral(SRC_TERMINALGRID_PATH));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "FAIL: cannot open %s\n", qUtf8Printable(f.fileName()));
        ++failures;
        return;
    }
    const QString src = QString::fromUtf8(f.readAll());

    // The constructor must read ANTS_OSC133_KEY exactly once via getenv.
    CHECK(src.contains(QStringLiteral("std::getenv(\"ANTS_OSC133_KEY\")")) ||
          src.contains(QStringLiteral("getenv(\"ANTS_OSC133_KEY\")")),
          "TerminalGrid constructor no longer reads ANTS_OSC133_KEY from env "
          "— either env knob renamed (update spec + docs) or removed (HMAC "
          "verifier won't activate in production)");

    // The setOsc133KeyForTest backdoor is test-only; it must not appear
    // anywhere outside the test harness or its own definition.
    int callSites = src.count(QStringLiteral("setOsc133KeyForTest"));
    // 1 occurrence = the inline definition in the .h header chain (this
    // file, terminalgrid.cpp, doesn't reference the inline helper).
    // > 1 in the .cpp would mean production code is calling the test
    // backdoor.
    CHECK(callSites == 0,
          "setOsc133KeyForTest is referenced from src/terminalgrid.cpp — "
          "it's a test-only backdoor and production code must not call it. "
          "Move the call into a test or use a real ANTS_OSC133_KEY-driven "
          "code path.");
}

}  // namespace

int main() {
    testVerifierOffPermissive();
    testVerifierOnAcceptsCorrect();
    testVerifierOnRejectsForged();
    testEnvKnobIsTheOnlyKnob();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All OSC 133 HMAC verification invariants hold.\n");
    return 0;
}
