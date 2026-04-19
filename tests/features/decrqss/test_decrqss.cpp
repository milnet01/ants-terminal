// Why this exists: locks the DECRQSS (DCS $ q Pt ST) response shapes
// added in 0.7.0 for `$qr` (DECSTBM), `$qm` (SGR), `$q q` (DECSCUSR),
// and the invalid-reply fallback `\eP0$r\e\\`. Also guards against
// the DECRQSS branch accidentally eating Sixel traffic. See spec.md.
//
// TerminalGrid::handleDcs is private; we drive it through the public
// processAction entry point with a VtAction::DcsEnd payload. SGR
// state is mutated via the VtParser for full end-to-end realism.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

struct Probe {
    TerminalGrid grid;
    VtParser     parser;
    std::string  capture;

    Probe()
        : grid(kRows, kCols),
          parser([this](const VtAction &a) { grid.processAction(a); }) {
        grid.setResponseCallback(
            [this](const std::string &s) { capture.append(s); });
    }

    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }

    void dcs(const std::string &payload) {
        VtAction a;
        a.type = VtAction::DcsEnd;
        a.oscString = payload;
        grid.processAction(a);
    }

    void clear() { capture.clear(); }
};

void printEscaped(FILE *f, const std::string &s) {
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F) std::fputc(c, f);
        else std::fprintf(f, "\\x%02x", c);
    }
}

int fail(const char *what, const std::string &got, const std::string &expected) {
    std::fprintf(stderr, "FAIL: %s\n  got:      \"", what);
    printEscaped(stderr, got);
    std::fprintf(stderr, "\"\n  expected: \"");
    printEscaped(stderr, expected);
    std::fprintf(stderr, "\"\n");
    return 1;
}

int failContains(const char *what, const std::string &got, const std::string &needle) {
    std::fprintf(stderr, "FAIL: %s\n  got:      \"", what);
    printEscaped(stderr, got);
    std::fprintf(stderr, "\"\n  wanted to contain: \"");
    printEscaped(stderr, needle);
    std::fprintf(stderr, "\"\n");
    return 1;
}

}  // namespace

int main() {
    int failures = 0;

    // ------------------------------------------------------------------
    // INV-1: $qr — DECSTBM. On a fresh 24-row grid, scroll region is
    //        full screen, which we report 1-indexed as 1;24.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.dcs("$qr");
        const std::string expected = "\x1BP1$r1;24r\x1B\\";
        if (p.capture != expected)
            failures += fail("INV-1 $qr DECSTBM response (fresh grid 24-row)",
                             p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-2: $qm — SGR with default attrs replies with just "0m".
    // ------------------------------------------------------------------
    {
        Probe p;
        p.dcs("$qm");
        const std::string expected = "\x1BP1$r0m\x1B\\";
        if (p.capture != expected)
            failures += fail("INV-2 $qm SGR response (default attrs)",
                             p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-3: $qm after CSI 1 m (bold) replies with "0;1m". Feeding the
    //        escape sequence through the parser exercises the full
    //        CSI dispatch path, not an internal setter.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.feed("\x1B[1m");   // bold on
        p.dcs("$qm");
        const std::string expected = "\x1BP1$r0;1m\x1B\\";
        if (p.capture != expected)
            failures += fail("INV-3 $qm with bold set (after CSI 1 m)",
                             p.capture, expected);
    }
    {
        // Combined 1;3;4 m set — assert all three flags appear in order.
        Probe p;
        p.feed("\x1B[1;3;4m");
        p.dcs("$qm");
        const std::string expected = "\x1BP1$r0;1;3;4m\x1B\\";
        if (p.capture != expected)
            failures += fail("INV-3 $qm with bold+italic+underline combined",
                             p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-4: $q q — DECSCUSR cursor shape. Fresh grid defaults to
    //        BlinkBlock = 0. The reply payload is "<digit> q".
    // ------------------------------------------------------------------
    {
        Probe p;
        p.dcs("$q q");
        const std::string expected = "\x1BP1$r0 q\x1B\\";
        if (p.capture != expected)
            failures += fail("INV-4 $q q DECSCUSR fresh grid (BlinkBlock=0)",
                             p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-5: unknown setting — exact invalid-reply bytes.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.dcs("$qX");
        const std::string expected = "\x1BP0$r\x1B\\";
        if (p.capture != expected)
            failures += fail("INV-5 $qX unknown setting invalid-reply",
                             p.capture, expected);
    }
    {
        Probe p;
        p.dcs("$qfoo");
        const std::string expected = "\x1BP0$r\x1B\\";
        if (p.capture != expected)
            failures += fail("INV-5 $qfoo unknown setting invalid-reply",
                             p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-6: Sixel dispatch must not be eaten by the DECRQSS branch.
    //        A payload that starts with something other than `$q`
    //        must route to Sixel. We assert by the *absence* of any
    //        `$r` in the capture — DECRQSS replies always contain
    //        `$r`, so no `$r` anywhere means DECRQSS didn't fire.
    // ------------------------------------------------------------------
    {
        Probe p;
        // Minimal Sixel: `q` starts data, `#0;2;100;0;0` defines color 0
        // as RGB(100%,0%,0%), then `#0` selects it and the data ends.
        p.dcs("q#0;2;100;0;0#0");
        if (p.capture.find("$r") != std::string::npos) {
            failures += failContains(
                "INV-6 Sixel payload must not trigger DECRQSS reply",
                p.capture, "(no $r substring expected)");
        }
        // Extra safety: assert we didn't emit the invalid-reply bytes.
        const std::string invalidReply = "\x1BP0$r\x1B\\";
        if (p.capture == invalidReply) {
            failures += fail("INV-6 Sixel payload must not emit invalid-reply",
                             p.capture, "(empty or non-DECRQSS output)");
        }
    }

    // ------------------------------------------------------------------
    // Bonus: structural check — every success reply begins `\eP1$r`
    //        and ends `\e\\`. Catches shape drift without pinning the
    //        exact payload contents.
    // ------------------------------------------------------------------
    auto structuralOk = [&](const std::string &s) {
        const std::string head = "\x1BP1$r";
        const std::string tail = "\x1B\\";
        if (s.size() < head.size() + tail.size()) return false;
        if (s.compare(0, head.size(), head) != 0) return false;
        if (s.compare(s.size() - tail.size(), tail.size(), tail) != 0) return false;
        return true;
    };
    {
        Probe p;
        p.dcs("$qr");
        if (!structuralOk(p.capture)) {
            failures += fail("structural: $qr reply must start \\eP1$r and end \\e\\\\",
                             p.capture,
                             "\\x1BP1$r...\\x1B\\\\");
        }
        if (p.capture.find("1;24r") == std::string::npos)
            failures += failContains("structural: $qr reply must contain 1;24r",
                                     p.capture, "1;24r");
    }

    if (failures == 0) {
        std::fprintf(stdout, "OK: decrqss invariants hold\n");
        return 0;
    }
    std::fprintf(stderr, "\ndecrqss: %d failure(s)\n", failures);
    return 1;
}
