// Feature-conformance test for ANTS-1284 token_usage MCP wiring.
// Source-grep approach mirrors mcp_plan_template_tool. See
// tests/features/mcp_token_usage_tool/spec.md for the contract.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Region: `// ANTS-1284 …` to the next `// ANTS-NNNN` or the
// `result["tools"] = tools;` line, whichever comes first. Same
// generic-anchor pattern as the plan_template test.
size_t tokenUsageBlockEnd(const std::string &ci, size_t start) {
    auto next = ci.find("// ANTS-", start + 1);  // any next anchor
    auto wall = ci.find("result[\"tools\"] = tools;", start);
    if (next == std::string::npos) return wall;
    if (wall == std::string::npos) return next;
    return next < wall ? next : wall;
}

}  // namespace

// REG-1
TEST(McpTokenUsageTool, ToolNameInToolsList) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("\"token_usage\""), std::string::npos)
        << "tool name token_usage missing from claudeintegration.cpp";
}

// REG-2
TEST(McpTokenUsageTool, ProviderRegisteredInMainWindow) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"token_usage\""),
              std::string::npos)
        << "registerToolProvider(\"token_usage\", ...) missing from "
           "mainwindow.cpp";
}

// REG-3
TEST(McpTokenUsageTool, CmdMethodDeclaredInHeader) {
    const std::string rch = slurp(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    EXPECT_NE(rch.find("cmdTokenUsage"), std::string::npos)
        << "cmdTokenUsage missing from remotecontrol.h";
}

// REG-4
TEST(McpTokenUsageTool, CmdMethodDefinedInCpp) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("RemoteControl::cmdTokenUsage"), std::string::npos)
        << "RemoteControl::cmdTokenUsage definition missing from "
           "remotecontrol.cpp";
}

// REG-5
TEST(McpTokenUsageTool, SchemaSetsAdditionalPropertiesFalse) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1284 — token_usage");
    ASSERT_NE(block_start, std::string::npos)
        << "// ANTS-1284 anchor missing — region scan can't run";
    const auto block_end = tokenUsageBlockEnd(ci, block_start);
    ASSERT_NE(block_end, std::string::npos);
    const std::string region = ci.substr(block_start, block_end - block_start);

    int count = 0;
    size_t pos = 0;
    while ((pos = region.find("additionalProperties", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_EQ(count, 1)
        << "expected exactly 1 additionalProperties=false in the "
           "token_usage registration block";
}

// REG-6
TEST(McpTokenUsageTool, SchemaListsOptionalArgsAndEmptyRequired) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1284 — token_usage");
    ASSERT_NE(block_start, std::string::npos);
    const auto block_end = tokenUsageBlockEnd(ci, block_start);
    ASSERT_NE(block_end, std::string::npos);
    const std::string region = ci.substr(block_start, block_end - block_start);

    for (const char *key : {"\"reset\"", "\"include_zero\""}) {
        EXPECT_NE(region.find(key), std::string::npos)
            << "schema arg " << key
            << " missing from token_usage registration block";
    }
    // No required args — the spec § 2.1 says all args are optional.
    // The schema initialises `required` to an empty QJsonArray().
    EXPECT_NE(region.find("schema[\"required\"] = QJsonArray()"),
              std::string::npos)
        << "expected empty `required` array (token_usage has no "
           "required args)";
}
