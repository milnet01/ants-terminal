// Feature-conformance test for ANTS-1290 plan_template MCP wiring.
// Source-grep approach mirrors mcp_verify_changes_tool. See
// tests/features/mcp_plan_template_tool/spec.md for the contract.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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


// Region: `// ANTS-1290 …` to the next `// ANTS-NNNN` or the
// `result["tools"] = tools;` line, whichever comes first. Same pattern
// as the verify_changes test was tightened to.
size_t planTemplateBlockEnd(const std::string &ci, size_t start) {
    auto next = ci.find("// ANTS-", start + 1);  // any next anchor
    auto wall = ci.find("result[\"tools\"] = tools;", start);
    if (next == std::string::npos) return wall;
    if (wall == std::string::npos) return next;
    return next < wall ? next : wall;
}

}  // namespace

// REG-1
TEST(McpPlanTemplateTool, ToolNameInToolsList) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("\"plan_template\""), std::string::npos)
        << "tool name plan_template missing from claudeintegration.cpp";
}

// REG-2
TEST(McpPlanTemplateTool, ProviderRegisteredInMainWindow) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"plan_template\""),
              std::string::npos)
        << "registerToolProvider(\"plan_template\", ...) missing from "
           "mainwindow.cpp";
}

// REG-3
TEST(McpPlanTemplateTool, CmdMethodDeclaredInHeader) {
    const std::string rch = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    EXPECT_NE(rch.find("cmdPlanTemplate"), std::string::npos)
        << "cmdPlanTemplate missing from remotecontrol.h";
}

// REG-4
TEST(McpPlanTemplateTool, CmdMethodDefinedInCpp) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("RemoteControl::cmdPlanTemplate"), std::string::npos)
        << "RemoteControl::cmdPlanTemplate definition missing from "
           "remotecontrol.cpp";
}

// REG-5
TEST(McpPlanTemplateTool, SchemaSetsAdditionalPropertiesFalse) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1290");
    ASSERT_NE(block_start, std::string::npos)
        << "// ANTS-1290 anchor missing — region scan can't run";
    const auto block_end = planTemplateBlockEnd(ci, block_start);
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
           "plan_template registration block";
}

// REG-6
TEST(McpPlanTemplateTool, SchemaListsRequiredAndOptionalArgs) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1290");
    ASSERT_NE(block_start, std::string::npos);
    const auto block_end = planTemplateBlockEnd(ci, block_start);
    ASSERT_NE(block_end, std::string::npos);
    const std::string region = ci.substr(block_start, block_end - block_start);

    for (const char *key : {"\"feature_name\"",
                            "\"goal\"",
                            "\"architecture\"",
                            "\"tech_stack\"",
                            "\"task_count_hint\"",
                            "\"includes_tests\"",
                            "\"ants_id\"",
                            "\"save\""}) {
        EXPECT_NE(region.find(key), std::string::npos)
            << "schema arg " << key
            << " missing from plan_template registration block";
    }
}
