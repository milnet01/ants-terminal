// Feature-conformance test for ANTS-1284 token_usage MCP wiring.
// Source-grep approach mirrors mcp_plan_template_tool. See
// tests/features/mcp_token_usage_tool/spec.md for the contract.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {


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
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("\"token_usage\""), std::string::npos)
        << "tool name token_usage missing from claudeintegration.cpp";
}

// REG-2
TEST(McpTokenUsageTool, ProviderRegisteredInMainWindow) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"token_usage\""),
              std::string::npos)
        << "registerToolProvider(\"token_usage\", ...) missing from "
           "mainwindow.cpp";
}

// REG-3
TEST(McpTokenUsageTool, CmdMethodDeclaredInHeader) {
    const std::string rch = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    EXPECT_NE(rch.find("cmdTokenUsage"), std::string::npos)
        << "cmdTokenUsage missing from remotecontrol.h";
}

// REG-4
TEST(McpTokenUsageTool, CmdMethodDefinedInCpp) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("RemoteControl::cmdTokenUsage"), std::string::npos)
        << "RemoteControl::cmdTokenUsage definition missing from "
           "remotecontrol.cpp";
}

// REG-5
TEST(McpTokenUsageTool, SchemaSetsAdditionalPropertiesFalse) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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

// ANTS-1355 REG-V2-1 — response builder emits envelope total_wrap_bytes.
TEST(McpTokenUsageTool, V2EnvelopeIncludesTotalWrapBytes) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("env[\"total_wrap_bytes\"]"), std::string::npos)
        << "cmdTokenUsage must surface total_wrap_bytes on the envelope";
}

// ANTS-1355 REG-V2-2 — per-tool entry emits wrap_bytes + duration fields.
TEST(McpTokenUsageTool, V2PerCallEntryIncludesWrapAndLatency) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    for (const char *key : {
            "c[\"wrap_bytes\"]",
            "c[\"duration_us_min\"]",
            "c[\"duration_us_max\"]",
            "c[\"duration_us_mean\"]",
         }) {
        EXPECT_NE(rc.find(key), std::string::npos)
            << "cmdTokenUsage per-call entry missing " << key;
    }
}

// ANTS-1355 REG-V2-3 — dispatch site computes wrapBytes + durUs and
// passes both to recordCall (5-argument v2 signature).
TEST(McpTokenUsageTool, V2DispatchSiteWiresFiveArgRecordCall) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    // Locate the `m_tokenUsage.recordCall(` invocation site.
    const auto recordCallPos = ci.find("m_tokenUsage.recordCall(");
    ASSERT_NE(recordCallPos, std::string::npos);
    // A v2 callsite reads as: recordCall(toolName, argBytes, outBytes,
    // wrapBytes, durUs). The literal tokens after `recordCall(` —
    // searched within a ~200-byte window from the call site — must
    // include each of these names.
    const std::string near =
        ci.substr(recordCallPos,
                  std::min<size_t>(ci.size() - recordCallPos, 200));
    for (const char *name : {"argBytes", "outBytes", "wrapBytes", "durUs"}) {
        EXPECT_NE(near.find(name), std::string::npos)
            << "recordCall site missing v2 argument: " << name;
    }
}

// ANTS-1355 REG-V2-4 — wrap delta is computed as outBytes - rawBytes.
TEST(McpTokenUsageTool, V2WrapDeltaComputedAsOutMinusRaw) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("outBytes - rawBytes"), std::string::npos)
        << "wrap_bytes must be computed as outBytes - rawBytes per "
           "ANTS-1355 INV-3";
}

// REG-6
TEST(McpTokenUsageTool, SchemaListsOptionalArgsAndEmptyRequired) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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
