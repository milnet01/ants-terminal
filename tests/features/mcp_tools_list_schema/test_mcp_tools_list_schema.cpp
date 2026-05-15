// Why this exists: locks the fix for the 2026-05-12 bug where six
// zero-arg MCP tools were emitted without an `inputSchema` field.
// Claude Code's Zod validator rejects the whole tools/list response
// when any entry is missing it, silently registering zero tools.
// See tests/features/mcp_tools_list_schema/spec.md.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

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

// INV-3 — emptySchema is constructed with type = "object".
// This is the shared sentinel used by all six zero-arg tools.
// If it's missing or lacks the type field the MCP validator rejects
// the whole response.
TEST(McpToolsListSchema, EmptySchemaTypeDeclared) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    expect(contains(ci, "emptySchema[\"type\"] = \"object\""),
           "INV-3",
           "emptySchema[\"type\"] = \"object\" not found in claudeintegration.cpp — "
           "zero-arg tools will be emitted without a valid inputSchema");

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " invariant(s) failed";
}

// INV-2 — each of the six zero-arg tools assigns inputSchema.
// One assertion per tool so the failure message names the offending tool.
TEST(McpToolsListSchema, ZeroArgToolsHaveInputSchema) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Pattern: <toolVar>["inputSchema"] = emptySchema;
    // The pre-fix code had no such assignments for these tools.
    struct { const char *tool; const char *assign; } checks[] = {
        { "get_cwd",          "cwdTool[\"inputSchema\"] = emptySchema"     },
        { "get_session_info", "sessionTool[\"inputSchema\"] = emptySchema" },
        { "get_last_command", "lastCmdTool[\"inputSchema\"] = emptySchema" },
        { "get_git_status",   "gitTool[\"inputSchema\"] = emptySchema"     },
        { "get_environment",  "envTool[\"inputSchema\"] = emptySchema"     },
        { "tab_list",         "tabListTool[\"inputSchema\"] = emptySchema" },
    };

    for (const auto &c : checks) {
        char label[128];
        std::snprintf(label, sizeof label, "INV-2 / %s", c.tool);
        char detail[256];
        std::snprintf(detail, sizeof detail,
                      "inputSchema not assigned for tool \"%s\" — "
                      "Claude Code Zod validator will reject all of tools/list",
                      c.tool);
        expect(contains(ci, c.assign), label, detail);
    }

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " zero-arg tool(s) missing inputSchema";
}

// INV-1 / INV-4 — every registered tool name appears alongside an
// inputSchema assignment. We verify that the count of tool names in
// the tools/list block matches the count of inputSchema assignments
// in the same source region so future tool additions cannot sneak in
// without a schema.
//
// Strategy: bound the search to the tools/list handler block (from
// "tools/list" to "tools/call"), then count "inputSchema" assignments
// and "tools.append(" calls — they must be equal.
TEST(McpToolsListSchema, AllRegisteredToolsHaveInputSchema) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Locate the tools/list block.
    const std::string kStart = "\"tools/list\"";
    const std::string kEnd   = "\"tools/call\"";
    const size_t startPos = ci.find(kStart);
    const size_t endPos   = (startPos == std::string::npos)
                                ? std::string::npos
                                : ci.find(kEnd, startPos);

    if (startPos == std::string::npos || endPos == std::string::npos) {
        FAIL() << "Cannot locate tools/list…tools/call block in claudeintegration.cpp; "
               << "check that the MCP handler is still present";
    }

    const std::string block = ci.substr(startPos, endPos - startPos);

    // Count tools.append( and ["inputSchema"] =
    auto countOccurrences = [](const std::string &hay,
                                const std::string &needle) -> int {
        int n = 0;
        size_t pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    };

    const int appendCount  = countOccurrences(block, "tools.append(");
    const int schemaCount  = countOccurrences(block, "[\"inputSchema\"] =");

    // Every tool appended must have had its inputSchema set.
    expect(appendCount > 0,
           "INV-1",
           "No tools.append() found in tools/list block — handler may have moved");
    expect(schemaCount > 0,
           "INV-1",
           "No inputSchema assignments found in tools/list block");
    if (appendCount > 0 && schemaCount > 0) {
        char detail[256];
        std::snprintf(detail, sizeof detail,
                      "tools.append count=%d but [\"inputSchema\"]= count=%d — "
                      "%d tool(s) are missing inputSchema",
                      appendCount, schemaCount,
                      appendCount - schemaCount);
        expect(appendCount == schemaCount, "INV-1 / INV-4", detail);
    }

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " invariant(s) failed";
}
