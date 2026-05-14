// Feature-conformance test for ANTS-1113 MCP wiring. Source-grep
// approach mirrors mcp_indie_review_tools: read claudeintegration.cpp
// + mainwindow.cpp + remotecontrol.h/.cpp and verify all 4 tool
// names are registered in each layer.

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

constexpr const char *kToolNames[4] = {
    "debt_sweep_scan",
    "debt_sweep_apply_fix",
    "debt_sweep_defer",
    "debt_sweep_triage_prompt",
};

constexpr const char *kCmdMethods[4] = {
    "cmdDebtSweepScan",
    "cmdDebtSweepApplyFix",
    "cmdDebtSweepDefer",
    "cmdDebtSweepTriagePrompt",
};

}  // namespace

TEST(McpDebtSweepTools, Inv12aAllToolNamesInToolsList) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    for (const char *name : kToolNames) {
        const std::string nameQuoted = std::string("\"") + name + "\"";
        EXPECT_NE(ci.find(nameQuoted), std::string::npos)
            << "tool name " << name << " missing from claudeintegration.cpp";
    }
}

TEST(McpDebtSweepTools, Inv12bAllProvidersRegisteredInMainWindow) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    for (const char *name : kToolNames) {
        const std::string call =
            std::string("registerToolProvider(\"") + name + "\"";
        EXPECT_NE(mw.find(call), std::string::npos)
            << "registerToolProvider(\"" << name
            << "\", ...) missing from mainwindow.cpp";
    }
}

TEST(McpDebtSweepTools, Inv12cAllCmdMethodsDeclaredInHeader) {
    const std::string rch = slurp(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    for (const char *m : kCmdMethods) {
        EXPECT_NE(rch.find(m), std::string::npos)
            << "method " << m << " missing from remotecontrol.h";
    }
}

TEST(McpDebtSweepTools, Inv12dAllCmdMethodsDefinedInCpp) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    for (const char *m : kCmdMethods) {
        const std::string defn = std::string("RemoteControl::") + m;
        EXPECT_NE(rc.find(defn), std::string::npos)
            << "definition RemoteControl::" << m
            << " missing from remotecontrol.cpp";
    }
}

TEST(McpDebtSweepTools, Inv12eAllSchemasUseAdditionalPropertiesFalse) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    // Region: from the first debt_sweep_* tool name to the close of
    // the last debt_sweep tool's block (its `tools.append(t);` call).
    // Terminating at the family boundary — not at `result["tools"]` —
    // keeps subsequently-inserted tool families (e.g. ANTS-1289
    // verify_changes) from leaking into the count.
    const auto block_start = ci.find("debt_sweep_scan");
    ASSERT_NE(block_start, std::string::npos);
    const auto last_block_pos = ci.find("debt_sweep_triage_prompt");
    ASSERT_NE(last_block_pos, std::string::npos);
    auto block_end = ci.find("tools.append(t);", last_block_pos);
    ASSERT_NE(block_end, std::string::npos);
    block_end += std::string("tools.append(t);").size();
    const std::string region = ci.substr(block_start, block_end - block_start);
    int count = 0;
    size_t pos = 0;
    while ((pos = region.find("additionalProperties", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_EQ(count, 4) << "expected 4 additionalProperties=false (one per tool)";
}
