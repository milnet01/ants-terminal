// ANTS-1578 — feature-conformance test for the discoverability hint
// folded into caller_cwd_info's main description. Source-scrape.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Extract the chunk of source between the "name = \"caller_cwd_info\""
// marker and the matching `tools.append(t);` so the test only checks
// strings inside the verb's own descriptor block.
std::string callerCwdInfoBlock(const std::string &src) {
    const std::string nameMarker = "t[\"name\"] = \"caller_cwd_info\"";
    const size_t open = src.find(nameMarker);
    if (open == std::string::npos) return std::string();
    const std::string endMarker = "tools.append(t);";
    const size_t close = src.find(endMarker, open);
    if (close == std::string::npos) return std::string();
    return src.substr(open, close - open);
}

}  // namespace

// INV-1 — the description block contains the trigger-phrase hint.
TEST(mcp_caller_cwd_info_description_hint, Inv1HintInDescription) {
    expect_reset();
    const std::string block =
        callerCwdInfoBlock(slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    expect(!block.empty(),
           "INV-1 setup: caller_cwd_info descriptor block located");
    expect(contains(block, "Use this FIRST when"),
           "INV-1: description carries the 'Use this FIRST when' "
           "discoverability hint (ANTS-1578)");
    expect(contains(block, "no_roadmap_loaded"),
           "INV-1: description names the no_roadmap_loaded trigger");
    expect(contains(block, "cwd_mismatch"),
           "INV-1: description names the cwd_mismatch trigger");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — the pre-existing selection_hint field is preserved.
// The two hints are parallel surfaces (description for callers that
// only read description text; selection_hint for the ANTS-1453
// per-tool selection_hint emission).
TEST(mcp_caller_cwd_info_description_hint, Inv2SelectionHintPreserved) {
    expect_reset();
    const std::string block =
        callerCwdInfoBlock(slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    expect(!block.empty(),
           "INV-2 setup: caller_cwd_info descriptor block located");
    expect(contains(block, "t[\"selection_hint\"] = QStringLiteral("),
           "INV-2: selection_hint field still assigned alongside the "
           "description (parallel surfaces, not a replacement)");
    EXPECT_EQ(0, expect_failures());
}
