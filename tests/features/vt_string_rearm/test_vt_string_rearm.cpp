// Feature-conformance test for spec.md (ANTS-1777) — locks the safe
// re-arm of string introducers after a string-terminating ESC.
//
// The 0.7.53 hardening (vt_osc_esc_discard) discards the byte after a
// string-terminating ESC so a hostile `ESC c` can't trigger RIS.
// ANTS-1777 re-arms ONLY when that byte is a string introducer
// (] P _ X ^), so back-to-back Sixel/Kitty/OSC sequences stop losing
// their second introducer — while every dangerous byte stays discarded.
//
// In-process: drives the scalar VtParser with crafted byte streams and
// records every dispatched VtAction. No TerminalGrid, no GUI deps.
// Streams are built by += of separate "\x1B"/introducer pieces so no
// \x escape gobbles a following hex digit.

#include "vtparser.h"

#include <cstdio>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

void expect(bool ok, const char *label, const char *detail = "") {
    if (!ok) {
        ADD_FAILURE() << label << (detail && *detail ? " — " : "")
                      << (detail ? detail : "");
    }
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

// Payloads of DcsEnd/ApcEnd/OscEnd all arrive in `oscString`.
std::vector<std::string> payloadsOfType(const std::vector<VtAction> &v,
                                        VtAction::Type t) {
    std::vector<std::string> out;
    for (const auto &a : v) if (a.type == t) out.push_back(a.oscString);
    return out;
}

// True if `c` appears in any Print action — handles both the scalar
// (codepoint) and the coalesced-run (printRun/printRunLen) shapes.
bool sawChar(const std::vector<VtAction> &v, char c) {
    for (const auto &a : v) {
        if (a.type != VtAction::Print) continue;
        if (a.printRun && a.printRunLen > 0) {
            for (int i = 0; i < a.printRunLen; ++i)
                if (a.printRun[i] == c) return true;
        }
        if (a.codepoint == static_cast<uint32_t>(static_cast<unsigned char>(c)))
            return true;
    }
    return false;
}

const char ESC = '\x1B';
const char ST8 = '\x9C';   // 8-bit String Terminator
const char BEL = '\x07';

// INV-1 — back-to-back DCS re-arms: two DcsEnd, both payloads intact,
// second payload NOT printed. Terminated by the 8-bit ST 0x9C.
TEST(VtStringRearm, dcsBackToBackReArms) {
    std::string s;
    s += ESC; s += 'P'; s += "AA";
    s += ESC; s += 'P'; s += "BB";
    s += ST8;
    const auto a = collectActions(s);

    const auto dcs = payloadsOfType(a, VtAction::DcsEnd);
    expect(dcs.size() == 2, "INV-1: two DcsEnd dispatched");
    if (dcs.size() == 2) {
        expect(dcs[0] == "AA", "INV-1: first DCS payload == AA");
        expect(dcs[1] == "BB", "INV-1: second DCS payload == BB (re-armed)");
    }
    expect(!sawChar(a, 'B'),
           "INV-1: second DCS payload BB must NOT Print (was re-armed, "
           "not spilled to Ground)");
}

// INV-2 — back-to-back APC re-arms (Kitty graphics shape).
TEST(VtStringRearm, apcBackToBackReArms) {
    std::string s;
    s += ESC; s += '_'; s += "AA";
    s += ESC; s += '_'; s += "BB";
    s += ESC; s += '\\';   // 7-bit ST
    const auto a = collectActions(s);

    const auto apc = payloadsOfType(a, VtAction::ApcEnd);
    expect(apc.size() == 2, "INV-2: two ApcEnd dispatched");
    if (apc.size() == 2) {
        expect(apc[0] == "AA", "INV-2: first APC payload == AA");
        expect(apc[1] == "BB", "INV-2: second APC payload == BB (re-armed)");
    }
}

// INV-3 — back-to-back OSC re-arms, terminated by BEL.
TEST(VtStringRearm, oscBackToBackReArms) {
    std::string s;
    s += ESC; s += ']'; s += "0;one";
    s += ESC; s += ']'; s += "0;two";
    s += BEL;
    const auto a = collectActions(s);

    const auto osc = payloadsOfType(a, VtAction::OscEnd);
    expect(osc.size() == 2, "INV-3: two OscEnd dispatched");
    if (osc.size() == 2) {
        expect(osc[0] == "0;one", "INV-3: first OSC payload intact");
        expect(osc[1] == "0;two", "INV-3: second OSC payload intact (re-armed)");
    }
}

// INV-4 — cross-type re-arm: OSC then DCS in one un-grounded run.
TEST(VtStringRearm, crossTypeReArm) {
    std::string s;
    s += ESC; s += ']'; s += "title";
    s += ESC; s += 'P'; s += "dcsdata";
    s += ST8;
    const auto a = collectActions(s);

    expect(countByType(a, VtAction::OscEnd) == 1, "INV-4: one OscEnd");
    expect(countByType(a, VtAction::DcsEnd) == 1, "INV-4: one DcsEnd");
    const auto osc = payloadsOfType(a, VtAction::OscEnd);
    const auto dcs = payloadsOfType(a, VtAction::DcsEnd);
    if (osc.size() == 1) expect(osc[0] == "title", "INV-4: OSC payload intact");
    if (dcs.size() == 1) expect(dcs[0] == "dcsdata", "INV-4: DCS payload intact");
}

// INV-5 — security preserved: a string + ESC + a dangerous selector
// still discards the selector (zero EscDispatch) and returns to Ground.
TEST(VtStringRearm, dangerousFinalsStillDiscarded) {
    const char dangerous[] = {'c', 'D', 'M', '7', '8', '=', '>', '('};
    for (char b : dangerous) {
        std::string s;
        s += ESC; s += 'P'; s += "payload";
        s += ESC; s += b;
        s += 'Z';   // trailing printable — must reach Ground/Print
        const auto a = collectActions(s);

        char detail[96];
        std::snprintf(detail, sizeof(detail), "selector='%c'", b);
        expect(countByType(a, VtAction::DcsEnd) == 1,
               "INV-5: exactly one DcsEnd", detail);
        expect(countByType(a, VtAction::EscDispatch) == 0,
               "INV-5: zero EscDispatch — dangerous selector discarded", detail);
        expect(sawChar(a, 'Z'),
               "INV-5: trailing Z Prints — parser returned to Ground", detail);
    }
}

// INV-6 — CSI stays discarded: the `[` after a string-ESC is NOT
// re-armed. Zero CsiDispatch over the whole stream (checked after the
// final `J`); `2` and `J` fall through to Ground as Print.
TEST(VtStringRearm, csiStaysDiscarded) {
    std::string s;
    s += ESC; s += ']'; s += "x";
    s += ESC; s += '[';
    s += '2'; s += 'J';
    const auto a = collectActions(s);

    expect(countByType(a, VtAction::OscEnd) == 1, "INV-6: one OscEnd");
    expect(countByType(a, VtAction::CsiDispatch) == 0,
           "INV-6: zero CsiDispatch — `[` discarded, not re-armed");
    expect(sawChar(a, '2') && sawChar(a, 'J'),
           "INV-6: `2`/`J` fall through to Ground as Print");
}

// INV-7 — SOS/PM re-arm is observable: the second `ESC X` re-enters
// IgnoreString, swallowing its payload; only the trailing Z Prints.
// (Pre-fix: the second `X` is discarded to Ground, so `bbb` Prints.)
TEST(VtStringRearm, sosPmReArmSwallowsPayload) {
    std::string s;
    s += ESC; s += 'X'; s += "aaa";
    s += ESC; s += 'X'; s += "bbb";
    s += ST8;
    s += 'Z';
    const auto a = collectActions(s);

    expect(countByType(a, VtAction::EscDispatch) == 0,
           "INV-7: zero EscDispatch (second X re-arms IgnoreString)");
    expect(!sawChar(a, 'b'),
           "INV-7: re-armed SOS/PM payload `bbb` is swallowed, not Printed");
    expect(sawChar(a, 'Z'),
           "INV-7: trailing Z Prints — parser returned to Ground");
}

}  // namespace
