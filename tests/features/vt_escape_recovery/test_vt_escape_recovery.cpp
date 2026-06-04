// Feature-conformance test for spec.md (ANTS-1998) — the VtParser Escape
// state catch-all. A byte ≥ 0x7F after ESC must abort the sequence to
// Ground, not leave the parser stuck so the next byte is mis-consumed as
// an ESC final.
//
// In-process: drives the scalar VtParser and records every VtAction.

#include "vtparser.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

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
const char DEL = '\x7F';

// INV-1 — ESC + DEL aborts the escape; the following 'M' is printed as text
// (NOT consumed as an ESC final / RI). Pre-fix the parser stayed in Escape
// after DEL and dispatched EscDispatch('M').
TEST(VtEscapeRecovery, Inv1DelAfterEscAbortsToGround) {
    std::string s;
    s += ESC; s += DEL; s += 'M';
    const auto a = collectActions(s);

    EXPECT_EQ(countByType(a, VtAction::EscDispatch), 0)
        << "DEL must abort the escape; 'M' must not become an ESC final";
    EXPECT_TRUE(sawChar(a, 'M'))
        << "'M' must Print as Ground text after the aborted escape";
}

// INV-2 — a well-formed escape still dispatches: ESC M (RI) → one
// EscDispatch with final 'M', and 'M' is NOT printed. Guards against the
// catch-all swallowing the normal 0x30-0x7E final path.
TEST(VtEscapeRecovery, Inv2WellFormedEscStillDispatches) {
    std::string s;
    s += ESC; s += 'M';
    const auto a = collectActions(s);

    ASSERT_EQ(countByType(a, VtAction::EscDispatch), 1)
        << "ESC M (RI) must still dispatch as an escape final";
    EXPECT_FALSE(sawChar(a, 'M')) << "the final byte must not also Print";
}

}  // namespace
