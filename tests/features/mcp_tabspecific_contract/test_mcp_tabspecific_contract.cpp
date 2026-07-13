// Feature-conformance test for ANTS-1415 Phase 3b — TabSpecific
// caller_cwd contract enforcement. Source-scrape against
// claudeintegration.cpp + the mcp-error-codes standard.
// See tests/features/mcp_tabspecific_contract/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {


bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — error-code doc row.
TEST(McpTabSpecificContract, Inv1ErrorCodeDocumented) {
    const std::string doc =
        ants_test::slurpFile(std::string(ANTS_SOURCE_DIR) + "/docs/standards/mcp-error-codes.md");
    ASSERT_FALSE(doc.empty()) << "could not read mcp-error-codes.md";
    EXPECT_TRUE(has(doc, "tab_or_cwd_required"))
        << "ANTS-1415 INV-1: tab_or_cwd_required row missing from "
           "mcp-error-codes.md § 3";
}

// INV-2 — refusal block present in the tools/call dispatch.
TEST(McpTabSpecificContract, Inv2RefusalBlockPresent) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("else if (method == \"tools/call\")");
    ASSERT_NE(pos, std::string::npos) << "tools/call branch missing";
    // 12000→17000 (ANTS-1901): the master-MCP `mcp_disabled` guard
    // inserted ahead of the caller_cwd block shifts the TabSpecific
    // refusal down ~1.1 KiB.
    const std::string region = cc.substr(pos, 17000);
    EXPECT_TRUE(has(region, "\"tab_or_cwd_required\""))
        << "ANTS-1415 INV-2: code literal missing";
    EXPECT_TRUE(has(region, "contract == CallerCwdContract::TabSpecific"))
        << "ANTS-1415 INV-2: TabSpecific contract guard missing";
    EXPECT_TRUE(has(region, "callerCwd.isEmpty() && !hasTab"))
        << "ANTS-1415 INV-2: refusal condition (empty cwd && no tab) missing";
}

// INV-3 — ordering: after caller_cwd_required, before the cache gate.
TEST(McpTabSpecificContract, Inv3OrderedAfterRequiredBeforeCache) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto reqAt   = cc.find("env[\"code\"]  = QStringLiteral(\"caller_cwd_required\")");
    const auto tabAt   = cc.find("env[\"code\"] = QStringLiteral(\"tab_or_cwd_required\")");
    const auto cacheAt = cc.find("!toolHandled && isIdempotentReadTool(toolName)");
    ASSERT_NE(reqAt,   std::string::npos) << "caller_cwd_required refusal not found";
    ASSERT_NE(tabAt,   std::string::npos) << "tab_or_cwd_required refusal not found";
    ASSERT_NE(cacheAt, std::string::npos) << "idempotent-read cache gate not found";
    EXPECT_LT(reqAt, tabAt)
        << "ANTS-1415 INV-3: TabSpecific refusal must come AFTER the "
           "Required refusal";
    EXPECT_LT(tabAt, cacheAt)
        << "ANTS-1415 INV-3: TabSpecific refusal must come BEFORE the "
           "idempotent-read cache gate (so refusals bypass the cache)";
}

// INV-4 — tabSpecificAcceptsTabIndex membership.
TEST(McpTabSpecificContract, Inv4TabRoutingHelperMembership) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("tabSpecificAcceptsTabIndex(const QString &toolName)");
    ASSERT_NE(pos, std::string::npos)
        << "ANTS-1415 INV-4: tabSpecificAcceptsTabIndex helper missing";
    const std::string body = cc.substr(pos, 220);
    EXPECT_TRUE(has(body, "\"get_text\""))
        << "ANTS-1415 INV-4: get_text must be a tab-routing tool";
    EXPECT_TRUE(has(body, "\"recent_errors\""))
        << "ANTS-1415 INV-4: recent_errors must be a tab-routing tool";
    // The four cwd-only tools must NOT appear in the helper body.
    for (const char *cwdOnly : {"\"get_scrollback\"", "\"get_last_command\"",
                                "\"get_environment\"", "\"get_cwd\""}) {
        EXPECT_FALSE(has(body, cwdOnly))
            << "ANTS-1415 INV-4: cwd-only tool " << cwdOnly
            << " must NOT be treated as tab-routing (a stray tab would "
               "bypass the gate)";
    }
}

// INV-8 — refusal sets dispatchResult for the failed-call accumulator.
TEST(McpTabSpecificContract, Inv8DispatchResultRouted) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    EXPECT_TRUE(has(cc, "dispatchResult = QStringLiteral(\"tab_or_cwd_required\")"))
        << "ANTS-1415 INV-8: refusal must set dispatchResult so "
           "recordDispatch counts it as failed";
}

// INV-9 — all six tools still classified TabSpecific (no reclassification).
TEST(McpTabSpecificContract, Inv9SixToolsStillTabSpecific) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(pos, std::string::npos) << "callerCwdContractFor missing";
    const auto end = cc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = cc.substr(pos, end - pos);
    for (const char *tool : {"\"get_text\"", "\"recent_errors\"",
                             "\"get_scrollback\"", "\"get_last_command\"",
                             "\"get_environment\"", "\"get_cwd\""}) {
        const auto p = body.find(tool);
        ASSERT_NE(p, std::string::npos)
            << "ANTS-1415 INV-9: " << tool << " missing from "
               "callerCwdContractFor";
        const auto eol = body.find('\n', p);
        const std::string line = body.substr(p, eol - p);
        EXPECT_TRUE(has(line, "C::TabSpecific;"))
            << "ANTS-1415 INV-9: " << tool
            << " must remain TabSpecific (no reclassification)";
    }
}
