// Feature-conformance test for ANTS-1289 verify_changes MCP wiring.
// Source-grep approach mirrors mcp_debt_sweep_tools. See
// tests/features/mcp_verify_changes_tool/spec.md for the contract.

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


}  // namespace

// REG-1
TEST(McpVerifyChangesTool, ToolNameInToolsList) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("\"verify_changes\""), std::string::npos)
        << "tool name verify_changes missing from claudeintegration.cpp";
}

// REG-2
TEST(McpVerifyChangesTool, ProviderRegisteredInMainWindow) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"verify_changes\""),
              std::string::npos)
        << "registerToolProvider(\"verify_changes\", ...) missing from "
           "mainwindow.cpp";
}

// REG-3
TEST(McpVerifyChangesTool, CmdMethodDeclaredInHeader) {
    const std::string rch = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    EXPECT_NE(rch.find("cmdVerifyChanges"), std::string::npos)
        << "cmdVerifyChanges missing from remotecontrol.h";
}

// REG-4
TEST(McpVerifyChangesTool, CmdMethodDefinedInCpp) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("RemoteControl::cmdVerifyChanges"), std::string::npos)
        << "RemoteControl::cmdVerifyChanges definition missing from "
           "remotecontrol.cpp";
}

// Region-scoped block-level checks (REG-5, REG-6). Region: from the
// `// ANTS-1289` anchor to the NEXT `// ANTS-NNNN` anchor (block-local),
// falling back to `result["tools"] = tools;` if there's no subsequent
// anchor. This tightening lets future tool registrations be inserted
// after the verify_changes block without polluting this scan region —
// see docs/specs/ANTS-1290.md § 5 for the MED-3 rationale.
namespace {
size_t verifyChangesBlockEnd(const std::string &ci, size_t start) {
    // Descriptor blocks in claudeintegration.cpp start at 16-space
    // indentation: `                // ANTS-NNNN`. Scan for the NEXT
    // such header after `// ANTS-1289` (verify_changes). Inline
    // comments that reference ANTS-NNNN at deeper indents don't
    // match — they're documentation, not block headers. This makes
    // the test resilient to descriptor blocks getting inserted
    // before plan_template (was ANTS-1290; ANTS-1351 inserted v1).
    const std::string anchor = "\n                // ANTS-";
    size_t pos = start + 12;  // step past "// ANTS-1289" itself
    const size_t hit = ci.find(anchor, pos);
    if (hit != std::string::npos) {
        // +1 skips the leading newline so the region ends just before
        // the next descriptor block's leading whitespace.
        return hit + 1;
    }
    return ci.find("result[\"tools\"] = tools;", start);
}
}  // namespace

TEST(McpVerifyChangesTool, SchemaSetsAdditionalPropertiesFalse) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1289");
    ASSERT_NE(block_start, std::string::npos)
        << "// ANTS-1289 anchor missing — region scan can't run";
    const auto block_end = verifyChangesBlockEnd(ci, block_start);
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
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("// ANTS-1289");
    ASSERT_NE(block_start, std::string::npos);
    const auto block_end = verifyChangesBlockEnd(ci, block_start);
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
