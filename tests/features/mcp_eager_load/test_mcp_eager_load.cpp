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

    // The curated high-frequency set (verbs that most directly replace
    // always-loaded Bash grep / Read / Edit + ROADMAP/CHANGELOG edits).
    for (const char *verb : {"workspace_search", "find_definition",
                             "file_outline", "read_region",
                             "roadmap_log", "changelog_log"}) {
        // Each appears in the file (as a tool name); the set lists all six.
        EXPECT_TRUE(has(ci, verb)) << "missing eager verb: " << verb;
    }
}
