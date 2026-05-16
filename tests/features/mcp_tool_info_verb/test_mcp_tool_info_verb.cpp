// ANTS-1399 — feature-conformance test for the tool_info MCP verb.
// Source-scrape pattern.

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

// INV-1 — tool_info declared as a tool in tools/list with
// required name + additionalProperties:false.
TEST(mcp_tool_info_verb, Inv1DescriptorDeclared) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "\"tool_info\""),
           "INV-1: tool_info name literal present in "
           "claudeintegration.cpp");
    expect(contains(ci, "ANTS-1399-INV-1"),
           "INV-1 anchor comment present");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — tools/list end stores tools into m_lastToolsList.
TEST(mcp_tool_info_verb, Inv2SnapshotPopulatedOnToolsList) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "ANTS-1399-INV-2"),
           "INV-2 anchor comment present");
    expect(contains(ci, "m_lastToolsList = tools"),
           "INV-2: snapshot assignment present at tools/list end");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — handler scans m_lastToolsList and emits the
// per-tool descriptor slice.
TEST(mcp_tool_info_verb, Inv3HandlerEmitsDescriptorSlice) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "ANTS-1399-INV-3"),
           "INV-3 anchor comment present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — unknown-name emits code:"unknown_tool" + available[].
TEST(mcp_tool_info_verb, Inv4UnknownToolEnvelope) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "\"unknown_tool\""),
           "INV-4: unknown_tool code literal present");
    expect(contains(ci, "\"available\""),
           "INV-4: available[] field emitted on the "
           "unknown-tool envelope");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — missing-name emits code:"missing_name".
TEST(mcp_tool_info_verb, Inv5MissingNameEnvelope) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "\"missing_name\""),
           "INV-5: missing_name code literal present");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — cold-snapshot emits code:"tools_not_ready".
TEST(mcp_tool_info_verb, Inv6ColdSnapshotEnvelope) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "\"tools_not_ready\""),
           "INV-6: tools_not_ready code literal present");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — classified ProcessGlobal in callerCwdContractFor.
TEST(mcp_tool_info_verb, Inv7ContractIsProcessGlobal) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto helperPos = ci.find(
        "callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(helperPos, std::string::npos)
        << "INV-7 precondition: callerCwdContractFor helper "
           "missing from claudeintegration.cpp";
    const auto helperEnd = ci.find("\n}\n", helperPos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body = ci.substr(helperPos,
                                       helperEnd - helperPos);
    const auto pos = body.find("\"tool_info\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-7: tool_info missing from callerCwdContractFor "
           "classification table";
    const auto eol = body.find('\n', pos);
    ASSERT_NE(eol, std::string::npos);
    const std::string line = body.substr(pos, eol - pos);
    expect(line.find("C::ProcessGlobal") != std::string::npos,
           "INV-7: tool_info must be ProcessGlobal — reads the "
           "process-wide tool registry, not per-tab state");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — handler dispatched inline alongside get_session_info,
// not via m_toolProviders.
TEST(mcp_tool_info_verb, Inv8DispatchedInline) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Locate the get_session_info dispatch branch. tool_info
    // must be in the same `if/else if` chain — i.e. dispatched
    // before the m_toolProviders lookup.
    const auto sessPos = ci.find(
        "toolName == \"get_session_info\"");
    ASSERT_NE(sessPos, std::string::npos)
        << "INV-8 precondition: get_session_info inline branch "
           "missing — claudeintegration.cpp may have regressed";
    // 4000-byte window covers a few following else-if branches.
    const std::string region = ci.substr(sessPos, 4000);
    expect(contains(region, "tool_info"),
           "INV-8: tool_info must be dispatched inline next to "
           "get_session_info (same toolHandled-conditional block)");
    EXPECT_EQ(0, expect_failures());
}
