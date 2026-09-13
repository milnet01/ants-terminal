// Feature-conformance test for spec.md (ANTS-5075).
//
// Feeds malformed CSI sequences to VtParser and inspects the actions it
// dispatches. No TerminalGrid, no GUI.

#include "vtparser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

struct Seen {
    std::vector<VtAction> csi;
    std::vector<char> executed;
    std::string printed;
};

Seen feed(const std::string &bytes) {
    Seen s;
    VtParser p([&s](const VtAction &a) {
        switch (a.type) {
        case VtAction::CsiDispatch: s.csi.push_back(a); break;
        case VtAction::Execute: s.executed.push_back(a.controlChar); break;
        case VtAction::Print:
            if (a.printRun) s.printed.append(a.printRun, a.printRunLen);
            else s.printed += static_cast<char>(a.codepoint);
            break;
        default: break;
        }
    });
    p.feed(bytes.data(), static_cast<int>(bytes.size()));
    return s;
}

}  // namespace

// INV-1
TEST(CsiIgnore, DelInsideCsiIsIgnored) {
    const Seen s = feed(std::string("\x1B[1\x7F" "2m"));
    ASSERT_EQ(s.csi.size(), 1u);
    EXPECT_EQ(s.csi[0].finalChar, 'm');
    EXPECT_EQ(s.csi[0].params, std::vector<int>({12}));
    EXPECT_EQ(s.printed, "");
}

// INV-2
TEST(CsiIgnore, PrivateMarkerAfterParamsDiscards) {
    const Seen s = feed("\x1B[1;<5mX");
    EXPECT_TRUE(s.csi.empty());
    EXPECT_EQ(s.printed, "X");
}

// INV-3
TEST(CsiIgnore, ParamByteAfterIntermediateDiscards) {
    const Seen s = feed("\x1B[!1pY");
    EXPECT_TRUE(s.csi.empty());
    EXPECT_EQ(s.printed, "Y");
}

// INV-4
TEST(CsiIgnore, C0InsideDiscardedSequenceExecutes) {
    const Seen s = feed("\x1B[1<\nm");
    EXPECT_TRUE(s.csi.empty());
    EXPECT_EQ(s.executed, std::vector<char>({'\n'}));
    EXPECT_EQ(s.printed, "");
}

// INV-5
TEST(CsiIgnore, LeadingPrivateMarkerStillDispatches) {
    const Seen s = feed("\x1B[?25h");
    ASSERT_EQ(s.csi.size(), 1u);
    EXPECT_EQ(s.csi[0].finalChar, 'h');
    EXPECT_EQ(s.csi[0].intermediate, "?");
    EXPECT_EQ(s.csi[0].params, std::vector<int>({25}));
}
