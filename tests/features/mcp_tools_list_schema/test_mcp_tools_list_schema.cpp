// Why this exists: locks the fix for the 2026-05-12 bug where six
// zero-arg MCP tools were emitted without an `inputSchema` field.
// Claude Code's Zod validator rejects the whole tools/list response
// when any entry is missing it, silently registering zero tools.
// See tests/features/mcp_tools_list_schema/spec.md.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


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
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

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
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Pattern: <toolVar>["inputSchema"] = <something>;
    // The pre-fix code had no such assignments for these tools. INV-2
    // is satisfied by *any* inputSchema assignment — several tools
    // shed the bare-emptySchema shape over time: get_cwd in ANTS-1391
    // (caller_cwd optional property); get_last_command, get_git_status,
    // get_environment in ANTS-1392 (caller_cwd anchor for the
    // terminal-state read verbs). For those, we check the substring
    // before the `=` so the test stays robust to the inputSchema body.
    // ANTS-1499 widened tab_list's schema from emptySchema to a
    // real {type:object, properties:{etag_match}} object so the tool
    // can opt into the "304 Not Modified" pattern. The INV-2 spirit
    // ("any inputSchema assignment, just don't ship the response
    // without one") still holds — the assertion drops the `emptySchema`
    // qualifier to match the new substring shape, same relaxation
    // get_cwd/get_environment/get_git_status received under ANTS-1391.
    struct { const char *tool; const char *assign; } checks[] = {
        { "get_cwd",          "cwdTool[\"inputSchema\"] = "       },
        { "get_session_info", "sessionTool[\"inputSchema\"] = emptySchema" },
        { "get_last_command", "lastCmdTool[\"inputSchema\"] = "   },
        { "get_git_status",   "gitTool[\"inputSchema\"] = "       },
        { "get_environment",  "envTool[\"inputSchema\"] = "       },
        { "tab_list",         "tabListTool[\"inputSchema\"] = "   },
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
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

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

// ANTS-1392 + ANTS-1393 — the registry-bridge lambdas in mainwindow.cpp
// must forward caller_cwd from `args` to the underlying handler.
// History: ANTS-1391 plumbed caller_cwd through cmdRoadmapQuery and
// added the schema property, but the registry lambda at
// mainwindow.cpp:3776 rebuilt `req` selectively (status + section only)
// and silently dropped caller_cwd — masking the fix. ANTS-1393 closed
// that and ANTS-1392 extended caller-anchoring to the terminal-state
// verbs via MainWindow::terminalForCaller().
TEST(McpToolsListSchema, RegistryLambdasForwardCallerCwd) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    // ANTS-1393 — roadmap_query lambda forwards caller_cwd into req.
    expect(contains(mw, "req[\"caller_cwd\"] = callerCwd"),
           "ANTS-1393",
           "roadmap_query registry lambda must forward caller_cwd "
           "to req — otherwise cmdRoadmapQuery never sees the field "
           "and the ANTS-1391 fix has no effect");

    // ANTS-1392 — MainWindow::terminalForCaller exists. The five
    // terminal-state lambdas (get_scrollback / get_last_command /
    // get_git_status / get_environment / get_text) route through it
    // so a Claude session in tab N gets its own terminal state.
    expect(contains(mw, "MainWindow::terminalForCaller"),
           "ANTS-1392",
           "MainWindow::terminalForCaller definition not found — "
           "terminal-state MCP verbs cannot route by caller_cwd");

    // ANTS-1392 — get_text lambda forwards caller_cwd into req so
    // cmdGetText's no-tab path can call terminalForCaller(callerCwd).
    expect(contains(mw, "req[\"caller_cwd\"] = cwdVal.toString()"),
           "ANTS-1392 / get_text",
           "get_text registry lambda must forward caller_cwd to req — "
           "otherwise cmdGetText's no-tab path resolves to focused tab");

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " invariant(s) failed";
}
