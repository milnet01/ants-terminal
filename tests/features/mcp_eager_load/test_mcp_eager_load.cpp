// Feature-conformance test for ANTS-2158 — eager-load the highest-frequency
// MCP verbs so they escape Claude Code's tool-search deferral (callable
// without a ToolSearch hop). The tools/list builder isn't invokable in
// isolation, so this is a source-grep wiring contract over the marking
// pass in claudeintegration.cpp (the shape other wiring tests use).
// See tests/features/mcp_eager_load/spec.md + ROADMAP ANTS-2158.

#include <gtest/gtest.h>
#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

namespace {
bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
}  // namespace

TEST(McpEagerLoad, AlwaysLoadMarkingWired) {
    const std::string ci =
        ants_test::slurpFile(std::string(ANTS_SOURCE_DIR)
                             + "/src/claudeintegration.cpp");

    // The exact, version-honoured field name + placement (Claude Code
    // v2.1.121+, code.claude.com/docs/en/mcp.md).
    EXPECT_TRUE(has(ci, "anthropic/alwaysLoad"))
        << "eager-load _meta field missing from tools/list builder";
    EXPECT_TRUE(has(ci, "kEagerVerbs"));

    // The curated set itself, read from its initializer: the grep / Read
    // substitutes are in it, and the rarely called write verbs are not —
    // each member costs context at every session start.
    const auto open = ci.find("kEagerVerbs = {");
    ASSERT_NE(open, std::string::npos);
    const auto close = ci.find("};", open);
    ASSERT_NE(close, std::string::npos);
    const std::string set = ci.substr(open, close - open);
    for (const char *verb : {"\"workspace_search\"", "\"find_definition\"",
                             "\"file_outline\"", "\"read_region\""}) {
        EXPECT_TRUE(has(set, verb)) << "missing eager verb: " << verb;
    }
    for (const char *verb : {"\"roadmap_log\"", "\"changelog_log\""}) {
        EXPECT_FALSE(has(set, verb))
            << "always-loaded but rarely called: " << verb;
    }
}
