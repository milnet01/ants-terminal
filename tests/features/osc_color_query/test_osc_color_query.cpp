// Why this exists: locks OSC 10/11/12 query-form response shape (xterm
// `rgb:RRRR/GGGG/BBBB` 16-bit) and the "set form is silently dropped"
// theme-injection guard added in 0.7.0. See spec.md.
//
// Drives TerminalGrid through its public processAction entry point
// with a VtAction::OscEnd payload — handleOsc is private, but the
// public dispatcher routes to it. The response callback is captured
// into a std::string for regex/exact comparison.

#include "terminalgrid.h"
#include "vtparser.h"

#include <cstdio>
#include <regex>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

struct Probe {
    TerminalGrid grid;
    std::string  capture;

    Probe() : grid(kRows, kCols) {
        grid.setResponseCallback(
            [this](const std::string &s) { capture.append(s); });
    }

    void osc(const std::string &payload) {
        VtAction a;
        a.type = VtAction::OscEnd;
        a.oscString = payload;
        grid.processAction(a);
    }

    void clear() { capture.clear(); }
};

int fail(const char *what, const std::string &got, const std::string &expected) {
    std::fprintf(stderr, "FAIL: %s\n  got:      \"", what);
    for (unsigned char c : got) {
        if (c >= 0x20 && c < 0x7F) std::fputc(c, stderr);
        else std::fprintf(stderr, "\\x%02x", c);
    }
    std::fprintf(stderr, "\"\n  expected: \"");
    for (unsigned char c : expected) {
        if (c >= 0x20 && c < 0x7F) std::fputc(c, stderr);
        else std::fprintf(stderr, "\\x%02x", c);
    }
    std::fprintf(stderr, "\"\n");
    return 1;
}

std::string fmtExpected(const char *oscNum, const QColor &c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "\x1B]%s;rgb:%04x/%04x/%04x\x1B\\",
                  oscNum,
                  c.red()   * 0x0101,
                  c.green() * 0x0101,
                  c.blue()  * 0x0101);
    return std::string(buf);
}

}  // namespace

int main() {
    int failures = 0;

    const QColor fg(0x12, 0x34, 0x56);
    const QColor bg(0xab, 0xcd, 0xef);

    // ------------------------------------------------------------------
    // INV-1: OSC 10;? responds with 16-bit fg encoding.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("10;?");
        const std::string expected = fmtExpected("10", fg);
        if (p.capture != expected)
            failures += fail("INV-1 OSC 10;? response shape", p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-2: OSC 11;? responds with 16-bit bg encoding.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("11;?");
        const std::string expected = fmtExpected("11", bg);
        if (p.capture != expected)
            failures += fail("INV-2 OSC 11;? response shape", p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-3: OSC 12;? (cursor) falls back to fg, still `rgb:` form,
    //        OSC number echoed as 12.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("12;?");
        const std::string expected = fmtExpected("12", fg);
        if (p.capture != expected)
            failures += fail("INV-3 OSC 12;? cursor fallback to fg", p.capture, expected);
    }

    // ------------------------------------------------------------------
    // INV-4: set form (no `?`) must NOT produce a response — theme
    //        injection guard. Tests both the `#rrggbb` shorthand and
    //        the long `rgb:` form that xterm accepts.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("10;#ff0000");
        if (!p.capture.empty())
            failures += fail("INV-4 OSC 10 set `#ff0000` must not respond",
                             p.capture, "");
    }
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("11;rgb:ff/00/00");
        if (!p.capture.empty())
            failures += fail("INV-4 OSC 11 set `rgb:ff/00/00` must not respond",
                             p.capture, "");
    }
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("12;#123456");
        if (!p.capture.empty())
            failures += fail("INV-4 OSC 12 set `#123456` must not respond",
                             p.capture, "");
    }

    // ------------------------------------------------------------------
    // INV-5: OSC 13 / other OSC color numbers are NOT handled via this
    //        branch; no response is emitted.
    // ------------------------------------------------------------------
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("13;?");
        if (!p.capture.empty())
            failures += fail("INV-5 OSC 13;? must not respond via 10/11/12 branch",
                             p.capture, "");
    }
    {
        Probe p;
        p.grid.setDefaultFg(fg);
        p.grid.setDefaultBg(bg);
        p.osc("14;?");
        if (!p.capture.empty())
            failures += fail("INV-5 OSC 14;? must not respond via 10/11/12 branch",
                             p.capture, "");
    }

    // ------------------------------------------------------------------
    // Bonus: verify the 16-bit hex encoding really does replicate low
    //        byte — regex-check each channel is of the form NNNN with
    //        the high and low byte equal.
    // ------------------------------------------------------------------
    {
        Probe p;
        const QColor probeFg(0x7a, 0x00, 0xff);
        p.grid.setDefaultFg(probeFg);
        p.osc("10;?");
        std::smatch m;
        std::regex re(R"(\x1B\]10;rgb:([0-9a-f]{4})/([0-9a-f]{4})/([0-9a-f]{4})\x1B\\)");
        if (!std::regex_match(p.capture, m, re)) {
            failures += fail("INV-1 16-bit regex shape",
                             p.capture,
                             "\\x1B]10;rgb:NNNN/NNNN/NNNN\\x1B\\\\");
        } else {
            auto hiLoMatch = [&](const std::string &hex) {
                return hex.substr(0, 2) == hex.substr(2, 2);
            };
            if (!hiLoMatch(m[1]) || !hiLoMatch(m[2]) || !hiLoMatch(m[3])) {
                std::fprintf(stderr,
                             "FAIL: 16-bit channels must replicate 8-bit low byte; got %s/%s/%s\n",
                             m[1].str().c_str(), m[2].str().c_str(), m[3].str().c_str());
                ++failures;
            }
        }
    }

    if (failures == 0) {
        std::fprintf(stdout, "OK: osc_color_query invariants hold\n");
        return 0;
    }
    std::fprintf(stderr, "\nosc_color_query: %d failure(s)\n", failures);
    return 1;
}
