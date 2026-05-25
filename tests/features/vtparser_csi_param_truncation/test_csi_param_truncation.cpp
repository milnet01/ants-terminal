// Feature-conformance test for spec.md (ANTS-1827).
//
// Drives VtParser scalar with crafted CSI sequences and inspects the
// dispatched VtAction's paramsTruncated flag. No TerminalGrid, no GUI.

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

const VtAction *firstCsi(const std::vector<VtAction> &v) {
    for (const auto &a : v) if (a.type == VtAction::CsiDispatch) return &a;
    return nullptr;
}

const VtAction *lastCsi(const std::vector<VtAction> &v) {
    const VtAction *found = nullptr;
    for (const auto &a : v) if (a.type == VtAction::CsiDispatch) found = &a;
    return found;
}

std::string manyParamSgr(int count) {
    std::string s = "\x1B[";
    for (int i = 0; i < count; ++i) s += "1;";
    s += "m";
    return s;
}

}  // namespace

// INV-1 — a normal CSI under the cap is not flagged.
TEST(CsiParamTruncation, NormalCsiNotTruncated) {
    const auto actions = collectActions("\x1B[1;31;42;4;7m");
    const VtAction *csi = firstCsi(actions);
    ASSERT_NE(csi, nullptr);
    EXPECT_FALSE(csi->paramsTruncated);
    EXPECT_EQ(csi->params.size(), 5u);
}

// INV-2 — a CSI exceeding 32 params is flagged and keeps exactly 32.
TEST(CsiParamTruncation, OverflowCsiFlagged) {
    const auto actions = collectActions(manyParamSgr(40));
    const VtAction *csi = firstCsi(actions);
    ASSERT_NE(csi, nullptr);
    EXPECT_TRUE(csi->paramsTruncated);
    EXPECT_EQ(csi->params.size(), 32u);
}

// INV-3 — the flag does not leak to the next CSI.
TEST(CsiParamTruncation, FlagResetsForNextCsi) {
    std::string s = manyParamSgr(40);  // truncated
    s += "\x1B[1;2H";                   // normal CSI right after
    const auto actions = collectActions(s);
    const VtAction *last = lastCsi(actions);
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->finalChar, 'H');
    EXPECT_FALSE(last->paramsTruncated);
}
