// Feature-conformance test for ANTS-1113 MCP wiring. Source-grep
// approach mirrors mcp_indie_review_tools: read claudeintegration.cpp
// + mainwindow.cpp + remotecontrol.h/.cpp and verify all 4 tool
// names are registered in each layer.

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
#ifndef SRC_MCPPROJECTION_CPP_PATH
#error "SRC_MCPPROJECTION_CPP_PATH compile definition required"
#endif

namespace {


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
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    for (const char *name : kToolNames) {
        const std::string nameQuoted = std::string("\"") + name + "\"";
        EXPECT_NE(ci.find(nameQuoted), std::string::npos)
            << "tool name " << name << " missing from claudeintegration.cpp";
    }
}

TEST(McpDebtSweepTools, Inv12bAllProvidersRegisteredInMainWindow) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
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
    const std::string rch = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    for (const char *m : kCmdMethods) {
        EXPECT_NE(rch.find(m), std::string::npos)
            << "method " << m << " missing from remotecontrol.h";
    }
}

TEST(McpDebtSweepTools, Inv12dAllCmdMethodsDefinedInCpp) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    for (const char *m : kCmdMethods) {
        const std::string defn = std::string("RemoteControl::") + m;
        EXPECT_NE(rc.find(defn), std::string::npos)
            << "definition RemoteControl::" << m
            << " missing from remotecontrol.cpp";
    }
}

TEST(McpDebtSweepTools, Inv12eAllSchemasUseAdditionalPropertiesFalse) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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

// Returns one tool's registration block: from its `t["name"] = "<tool>"`
// anchor to that block's own `tools.append(t);`. Anchoring on the name
// assignment (not a bare tool-name substring) avoids matching a mention of
// the tool inside a *sibling* tool's description/selection_hint; terminating
// at the block's own append is the same robust boundary INV-12e uses.
static std::string schemaBlock(const std::string &ci, const char *tool) {
    const auto start =
        ci.find(std::string("t[\"name\"] = \"") + tool + "\"");
    if (start == std::string::npos) return {};
    auto end = ci.find("tools.append(t);", start);
    if (end == std::string::npos) end = ci.size();
    return ci.substr(start, end - start);
}

TEST(McpDebtSweepTools, Ants3345ScanPaginationSchema) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const std::string scan = schemaBlock(ci, "debt_sweep_scan");
    ASSERT_FALSE(scan.empty());
    EXPECT_NE(scan.find("\"limit\""), std::string::npos)
        << "scan schema missing limit prop (ANTS-3345)";
    EXPECT_NE(scan.find("\"offset\""), std::string::npos)
        << "scan schema missing offset prop (ANTS-3345)";
}

TEST(McpDebtSweepTools, Ants3345ScanEmitsPaginationEnvelope) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    // The cmdDebtSweepScan envelope now carries the pagination fields.
    EXPECT_NE(rc.find("has_more"), std::string::npos);
    EXPECT_NE(rc.find("next_offset"), std::string::npos);
}

TEST(McpDebtSweepTools, Ants3345ScanIsOffloadEligible) {
    const std::string mp = ants_test::slurpFile(SRC_MCPPROJECTION_CPP_PATH);
    ASSERT_FALSE(mp.empty());
    EXPECT_NE(mp.find("debt_sweep_scan"), std::string::npos)
        << "debt_sweep_scan not in isOffloadEligible allowlist (ANTS-3345)";
}

TEST(McpDebtSweepTools, Ants3346DeferTriageGate) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    ASSERT_FALSE(rc.empty());
    const std::string defer = schemaBlock(ci, "debt_sweep_defer");
    ASSERT_FALSE(defer.empty());
    EXPECT_NE(defer.find("\"triaged\""), std::string::npos)
        << "defer schema missing triaged prop (ANTS-3346)";
    EXPECT_NE(rc.find("needs_triage"), std::string::npos)
        << "cmdDebtSweepDefer missing needs_triage refusal (ANTS-3346)";
    EXPECT_NE(rc.find("evaluateTriageGate"), std::string::npos)
        << "cmdDebtSweepDefer does not call evaluateTriageGate (ANTS-3346)";
}
