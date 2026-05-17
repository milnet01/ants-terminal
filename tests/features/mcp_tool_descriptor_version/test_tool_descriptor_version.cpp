// ANTS-1354 — feature-conformance test for the `version` field on
// every MCP tool descriptor. Source-scrape style.

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

}  // namespace

// INV-1 — finalisation loop sets version on every descriptor.
TEST(mcp_tool_descriptor_version, Inv1FinalisationLoopPresent) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "ANTS-1354"),
           "INV-1: ANTS-1354 anchor present in tools/list builder");
    expect(contains(cpp,
                    "t[QStringLiteral(\"version\")] = "
                    "QStringLiteral(\"1.0\")"),
           "INV-1: default version 1.0 emitted for every tool entry");
}

// INV-2 — SemVer-of-tools policy documented at the call site.
TEST(mcp_tool_descriptor_version, Inv2SemverPolicyDocumented) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "SemVer-\n                // of-tools") ||
           contains(cpp, "SemVer-of-tools policy") ||
           contains(cpp, "SemVer-\n                // of tools"),
           "INV-2: SemVer-of-tools policy comment present");
    expect(contains(cpp, "MAJOR") && contains(cpp, "MINOR") &&
           contains(cpp, "PATCH"),
           "INV-2: comment names MAJOR / MINOR / PATCH semantics");
}

// INV-3 — per-tool override is non-destructive.
TEST(mcp_tool_descriptor_version, Inv3PerToolOverrideNonDestructive) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "if (!t.contains(QStringLiteral(\"version\")))"),
           "INV-3: default-fill loop guards on the version field "
           "being absent, so a tool that explicitly set its own "
           "version isn't overwritten");
}
