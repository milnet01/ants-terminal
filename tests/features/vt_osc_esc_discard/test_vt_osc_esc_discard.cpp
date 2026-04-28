// Feature-conformance test for spec.md — locks the 0.7.53 fix for
// the OSC-string ESC-discard parser invariant.
//
// Asserts that ESC mid-stream inside OSC / DCS / APC / SOS/PM string
// states ends that string and consumes the trailing byte without
// dispatching it as a fresh ESC sequence (RIS / IND / DECSC / etc.
// — see spec for the RCE-adjacent fallout shape).
//
// In-process: drives VtParser scalar with crafted byte sequences and
// records every dispatched VtAction into a vector. No TerminalGrid,
// no GUI deps.

#include "vtparser.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const char *detail = "") {
    std::fprintf(stderr, "[%-72s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 (detail && *detail) ? " — " : "",
                 detail ? detail : "");
    if (!ok) ++g_failures;
}

std::vector<VtAction> collectActions(const std::string &bytes) {
    std::vector<VtAction> out;
    VtParser p([&out](const VtAction &a) { out.push_back(a); });
    p.feed(bytes.data(), static_cast<int>(bytes.size()));
    return out;
}

int countByType(const std::vector<VtAction> &v, VtAction::Type t) {
    int n = 0;
    for (const auto &a : v) if (a.type == t) ++n;
    return n;
}

const VtAction *findByType(const std::vector<VtAction> &v, VtAction::Type t) {
    for (const auto &a : v) if (a.type == t) return &a;
    return nullptr;
}

// INV-1 — OSC string with ESC + X (X != backslash) discards X.
// Tries multiple X values to cover the RCE-adjacent escape selectors.
void testInv1_oscEscX_discardsX() {
    struct Case { const char *name; char escByte; };
    Case cases[] = {
        {"INV-1: OSC + ESC c (would-be RIS) — c discarded", 'c'},
        {"INV-1: OSC + ESC D (would-be IND) — D discarded", 'D'},
        {"INV-1: OSC + ESC M (would-be RI) — M discarded",  'M'},
        {"INV-1: OSC + ESC 7 (would-be DECSC) — 7 discarded", '7'},
        {"INV-1: OSC + ESC 8 (would-be DECRC) — 8 discarded", '8'},
        {"INV-1: OSC + ESC = (would-be DECPAM) — = discarded", '='},
        {"INV-1: OSC + ESC > (would-be DECPNM) — > discarded", '>'},
    };

    for (const auto &c : cases) {
        std::string stream = "\x1B]0;hello\x1B";
        stream.push_back(c.escByte);
        const auto actions = collectActions(stream);

        const int oscEnds = countByType(actions, VtAction::OscEnd);
        const int escDispatches = countByType(actions, VtAction::EscDispatch);
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "got oscEnds=%d escDispatches=%d (total %zu actions)",
                      oscEnds, escDispatches, actions.size());
        expect(oscEnds == 1 && escDispatches == 0, c.name, detail);

        // The OSC body must arrive intact (the body is what's BEFORE
        // the trailing ESC); the trailing ESC must NOT have been
        // appended to it.
        const VtAction *osc = findByType(actions, VtAction::OscEnd);
        if (osc) {
            const bool bodyClean = (osc->oscString == "0;hello");
            expect(bodyClean, c.name,
                   "OSC body should be \"0;hello\" — the trailing ESC must "
                   "not leak into the dispatched payload");
        }
    }
}

// INV-2 — `ESC \` (legitimate ST) goes through the same path. The
// `\` is consumed and the parser returns to Ground; no garbage
// printed and no escape dispatched.
void testInv2_oscEscBackslash_isStLikeOther() {
    const std::string stream = "\x1B]52;c;dGVzdA==\x1B\\";
    const auto actions = collectActions(stream);
    const int oscEnds = countByType(actions, VtAction::OscEnd);
    const int escDispatches = countByType(actions, VtAction::EscDispatch);
    const int prints = countByType(actions, VtAction::Print);

    expect(oscEnds == 1, "INV-2: ESC \\ ST emits exactly 1 OscEnd");
    expect(escDispatches == 0,
           "INV-2: ESC \\ ST emits NO EscDispatch (the \\ is consumed)");
    expect(prints == 0,
           "INV-2: ESC \\ ST does not Print the \\ to the grid");

    const VtAction *osc = findByType(actions, VtAction::OscEnd);
    if (osc) {
        const bool bodyClean = (osc->oscString == "52;c;dGVzdA==");
        expect(bodyClean,
               "INV-2: OSC body before ESC \\ ST is preserved verbatim");
    }
}

// INV-3 — DCS / APC string states share the same fix. We test DCS
// here; APC has the same code path. SOS/PM (IgnoreString) also
// shares the path but doesn't dispatch a typed end action — it just
// returns to Ground silently (and the test for that is "no
// EscDispatch" + parser ready to accept new input, indirectly
// asserted via INV-1 family).
void testInv3_dcsEscX_discardsX() {
    // ESC P 1;2|payload ESC c — DCS sequence ending in ESC c.
    // Note: split the string so `\x1B` doesn't gobble the trailing
    // 'c' as a hex digit (out-of-range warning + wrong byte stream).
    const std::string stream = std::string("\x1BP" "1;2|payload\x1B") + "c";
    const auto actions = collectActions(stream);
    const int dcsEnds = countByType(actions, VtAction::DcsEnd);
    const int escDispatches = countByType(actions, VtAction::EscDispatch);

    expect(dcsEnds == 1,
           "INV-3: DCS + ESC c emits exactly 1 DcsEnd");
    expect(escDispatches == 0,
           "INV-3: DCS + ESC c emits NO EscDispatch (c consumed)");
}

// INV-4 — ordinary (non-ESC, non-BEL, non-ST) bytes inside the OSC
// body still accumulate into the dispatched payload. Defensively
// tests that the new OscStringEsc state didn't accidentally swallow
// the OscString-side accumulation.
void testInv4_oscBodyStillAccumulates() {
    const std::string body =
        "8;;https://example.com/very/long/url?query=foo&bar=baz";
    const std::string stream = "\x1B]" + body + "\x07";
    const auto actions = collectActions(stream);

    const VtAction *osc = findByType(actions, VtAction::OscEnd);
    expect(osc != nullptr,
           "INV-4: OscEnd dispatched after BEL terminator");
    if (osc) {
        const bool bodyMatches = (osc->oscString == body);
        expect(bodyMatches,
               "INV-4: full OSC body preserved byte-for-byte");
    }
}

// INV-5 — after `OscString + ESC + X`, the parser is back in
// Ground state and accepts new bytes normally. Regressions could
// strand us in OscStringEsc forever (or worse, in an undefined
// state); test by feeding plain printable text after the discard
// and asserting it Prints.
void testInv5_parserReturnsToGroundAfterEscDiscard() {
    // Note the explicit `\x1B` `c` split — `"\x1Bc"` would parse as
    // a single hex literal `\x1Bc` (out-of-range warning) rather than
    // the two-byte ESC + 'c' sequence we want. Same trick used below.
    const std::string stream = std::string("\x1B]0;hello\x1B") + "cABC";
    const auto actions = collectActions(stream);

    const int oscEnds = countByType(actions, VtAction::OscEnd);
    const int escDispatches = countByType(actions, VtAction::EscDispatch);
    const int prints = countByType(actions, VtAction::Print);

    expect(oscEnds == 1, "INV-5: OSC ended cleanly");
    expect(escDispatches == 0, "INV-5: no rogue EscDispatch");
    // Print actions arrive as one-codepoint dispatches in the scalar
    // path (or one Print containing the run on the SIMD path); both
    // shapes are valid. We just assert that *some* Prints arrived
    // carrying ABC's bytes.
    bool sawA = false, sawB = false, sawC = false;
    for (const auto &a : actions) {
        if (a.type != VtAction::Print) continue;
        // Handle either scalar shape (single codepoint) or run shape
        // (printRun + printRunLen bytes pointing into feed buffer).
        if (a.printRun && a.printRunLen > 0) {
            for (int i = 0; i < a.printRunLen; ++i) {
                char c = a.printRun[i];
                if (c == 'A') sawA = true;
                if (c == 'B') sawB = true;
                if (c == 'C') sawC = true;
            }
        }
        if (a.codepoint == 'A') sawA = true;
        if (a.codepoint == 'B') sawB = true;
        if (a.codepoint == 'C') sawC = true;
    }
    expect(sawA && sawB && sawC,
           "INV-5: ABC after the ESC-discard reaches Print actions",
           "parser stranded in OscStringEsc?");
    expect(prints >= 1, "INV-5: at least one Print dispatched");
}

}  // namespace

int main() {
    testInv1_oscEscX_discardsX();
    testInv2_oscEscBackslash_isStLikeOther();
    testInv3_dcsEscX_discardsX();
    testInv4_oscBodyStillAccumulates();
    testInv5_parserReturnsToGroundAfterEscDiscard();
    return g_failures == 0 ? 0 : 1;
}
