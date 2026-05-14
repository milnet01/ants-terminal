// Feature-conformance test for ANTS-1289 verify_changes MCP wiring.
// Source-grep approach mirrors mcp_debt_sweep_tools. See
// tests/features/mcp_verify_changes_tool/spec.md for the contract.

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

}  // namespace

// REG-1
TEST(McpVerifyChangesTool, ToolNameInToolsList) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("\"verify_changes\""), std::string::npos)
        << "tool name verify_changes missing from claudeintegration.cpp";
}

// REG-2
TEST(McpVerifyChangesTool, ProviderRegisteredInMainWindow) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"verify_changes\""),
              std::string::npos)
        << "registerToolProvider(\"verify_changes\", ...) missing from "
           "mainwindow.cpp";
}

// REG-3
TEST(McpVerifyChangesTool, CmdMethodDeclaredInHeader) {
    const std::string rch = slurp(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    EXPECT_NE(rch.find("cmdVerifyChanges"), std::string::npos)
        << "cmdVerifyChanges missing from remotecontrol.h";
}

// REG-4
TEST(McpVerifyChangesTool, CmdMethodDefinedInCpp) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("RemoteControl::cmdVerifyChanges"), std::string::npos)
        << "RemoteControl::cmdVerifyChanges definition missing from "
           "remotecontrol.cpp";
}

// Region-scoped block-level checks (REG-5, REG-6). Region: from the
// `// ANTS-1289` anchor to the next `result["tools"] = tools;`.
// Future tools added after this one should introduce their own
// `// ANTS-NNNN` anchor so a similar region-scoped test can ride.
TEST(McpVerifyChangesTool, SchemaSetsAdditionalPropertiesFalse) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1289");
    ASSERT_NE(block_start, std::string::npos)
        << "// ANTS-1289 anchor missing — region scan can't run";
    auto block_end = ci.find("result[\"tools\"] = tools;", block_start);
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
           "verify_changes registration block";
}

TEST(McpVerifyChangesTool, SchemaListsOptionalArgs) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1289");
    ASSERT_NE(block_start, std::string::npos);
    auto block_end = ci.find("result[\"tools\"] = tools;", block_start);
    ASSERT_NE(block_end, std::string::npos);
    const std::string region = ci.substr(block_start, block_end - block_start);

    // Each optional arg must appear as a quoted properties key.
    for (const char *key : {"\"gates\"",
                            "\"max_log_lines\"",
                            "\"timeout_sec\""}) {
        EXPECT_NE(region.find(key), std::string::npos)
            << "optional schema arg " << key
            << " missing from verify_changes registration block";
    }
}
