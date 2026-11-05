// Feature-conformance test for ANTS-1404 Phase 3a — per-tool
// caller_cwd contract enforcement.
// See tests/features/mcp_caller_cwd_contracts/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef ANTS_CLAUDEINTEGRATION_SOURCES
#error "ANTS_CLAUDEINTEGRATION_SOURCES compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
#endif

namespace {

// ANTS-3681 — the brace-matched tools/call arm of the method dispatch chain.
// It replaces byte windows that were widened five times (9000 → 23000) as
// the arm grew; "" when the arm is missing.
std::string toolsCallBranch(const std::string &cc) {
    return ants_test::slurpFunctionBody(cc, "else if (method == \"tools/call\")");
}

}  // namespace

// CLS-1 — Required group classification.
TEST(McpCallerCwdContracts, RequiredToolsClassified) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    // Locate the helper definition.
    const auto pos = cc.find("callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(pos, std::string::npos)
        << "callerCwdContractFor helper missing";
    const auto end = cc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = cc.substr(pos, end - pos);

    for (const auto *tool : {
            "\"last_audit_summary\"",
            "\"git_state\"",
            "\"verify_changes\"",
            // ANTS-1430 — project_layout reads tenant-hashed storage.
            "\"project_layout\"",
            // ANTS-1435 — session_memory joins Required (dispatcher
            // refuses empty caller_cwd upstream; handler still has
            // a body-level cwd_missing for the IPC path).
            "\"session_memory\"",
            // ANTS-1424 — roadmap_log mutates ROADMAP under caller cwd.
            "\"roadmap_log\""}) {
        const auto p = body.find(tool);
        ASSERT_NE(p, std::string::npos)
            << "ANTS-1404 CLS-1: tool " << tool << " missing from "
               "callerCwdContractFor classification table";
        // The classification line must end with `C::Required;`.
        const auto eol = body.find('\n', p);
        ASSERT_NE(eol, std::string::npos);
        const std::string line = body.substr(p, eol - p);
        EXPECT_NE(line.find("C::Required;"), std::string::npos)
            << "ANTS-1404 CLS-1: " << tool << " not classified Required";
    }
}

// CLS-2 — TabSpecific + ProcessGlobal groups present.
TEST(McpCallerCwdContracts, OtherCategoriesPresent) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(pos, std::string::npos);
    const auto end = cc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = cc.substr(pos, end - pos);

    EXPECT_NE(body.find("C::TabSpecific;"), std::string::npos)
        << "ANTS-1404 CLS-2: TabSpecific category missing";
    EXPECT_NE(body.find("C::ProcessGlobal;"), std::string::npos)
        << "ANTS-1404 CLS-2: ProcessGlobal category missing";
    // Default-Optional behaviour at the end.
    EXPECT_NE(body.find("return C::Optional;"), std::string::npos)
        << "ANTS-1404 INV-4: missing default-to-Optional return";
}

// DISP-1 — dispatch site calls callerCwdContractFor.
TEST(McpCallerCwdContracts, DispatchSiteInvokesContractCheck) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    // Find the tools/call body.
    const std::string region = toolsCallBranch(cc);
    ASSERT_FALSE(region.empty())
        << "tools/call dispatch branch missing";
    EXPECT_NE(region.find("callerCwdContractFor(toolName)"),
              std::string::npos)
        << "ANTS-1404 DISP-1: dispatcher must consult "
           "callerCwdContractFor before the cache + provider path";
}

// DISP-2 — refusal envelope shape.
TEST(McpCallerCwdContracts, RefusalEnvelopeShape) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string region = toolsCallBranch(cc);
    ASSERT_FALSE(region.empty());
    EXPECT_NE(region.find("\"caller_cwd_required\""), std::string::npos)
        << "ANTS-1404 DISP-2: code:\"caller_cwd_required\" literal "
           "missing from refusal envelope";
    // Envelope sets ok=false explicitly.
    EXPECT_NE(region.find("env[\"ok\"]    = false"),
              std::string::npos)
        << "ANTS-1404 DISP-2: refusal envelope must set ok=false";
}

// DISP-3 — cacheable gating on !toolHandled.
TEST(McpCallerCwdContracts, RefusalBypassesCache) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string region = toolsCallBranch(cc);
    ASSERT_FALSE(region.empty());
    // The cacheable assignment must depend on !toolHandled so a
    // refusal-set toolHandled value short-circuits the cache lookup.
    EXPECT_NE(region.find("!toolHandled && isIdempotentReadTool(toolName)"),
              std::string::npos)
        << "ANTS-1404 INV-7: cacheable must be gated on "
           "!toolHandled so refusals bypass the cache";
}

// DISP-5 (ANTS-1454) — refusal branch sets dispatchResult so
// recordDispatch routes the call into the failed-call accumulator.
// Pre-1454 the branch inherited the "ok" default and
// token_usage.recordCall counted refusals as successes.
TEST(McpCallerCwdContracts, RefusalSetsDispatchResult) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string region = toolsCallBranch(cc);
    ASSERT_FALSE(region.empty());
    // Locate the ANTS-1404 refusal block (anchored on the literal
    // code string already asserted by DISP-2) and walk forward to
    // the toolHandled = true that closes the branch; the
    // dispatchResult assignment must appear in the same block.
    const auto refusalAt =
        region.find("env[\"code\"]  = QStringLiteral(\"caller_cwd_required\")");
    ASSERT_NE(refusalAt, std::string::npos)
        << "ANTS-1454 DISP-5: refusal envelope literal not found";
    const auto branchEnd = region.find("}\n", refusalAt);
    ASSERT_NE(branchEnd, std::string::npos)
        << "ANTS-1454 DISP-5: refusal branch close brace not found";
    const std::string branch = region.substr(
        refusalAt, branchEnd - refusalAt);
    EXPECT_NE(branch.find(
                  "dispatchResult = "
                  "QStringLiteral(\"caller_cwd_required\")"),
              std::string::npos)
        << "ANTS-1454 DISP-5: ANTS-1404 refusal branch must set "
           "dispatchResult = \"caller_cwd_required\" so "
           "token_usage's recordCall sees succeeded=false (the "
           "pre-1454 default \"ok\" masked failed-call cost).";
}

// DISP-4 — provider-dispatch guard widened to !toolHandled.
TEST(McpCallerCwdContracts, ProviderDispatchGuardedByToolHandled) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string region = toolsCallBranch(cc);
    ASSERT_FALSE(region.empty());
    // The provider-dispatch block historically guarded on `!cachedHit`;
    // ANTS-1404 widens that to `!toolHandled` so refusals also
    // short-circuit the provider lambda invocation.
    EXPECT_NE(region.find("if (!toolHandled) {"), std::string::npos)
        << "ANTS-1404 DISP-4: provider-dispatch block must guard on "
           "!toolHandled, not !cachedHit, so refusals don't fall "
           "through to the provider lambda";
}

// Header surface — CallerCwdContract enum + helper visible.
TEST(McpCallerCwdContracts, HeaderSurface) {
    // ANTS-4932 § 2.3 — the enum is defined in mcptoolsink.h and aliased into
    // ClaudeIntegration, so the surface is the two headers together.
    const std::string h = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH) +
                          ants_test::slurpFile(SRC_MCP_TOOL_SINK_H_PATH);
    ASSERT_FALSE(h.empty());
    EXPECT_NE(h.find("enum class CallerCwdContract"), std::string::npos)
        << "ANTS-1404: CallerCwdContract enum missing from header";
    EXPECT_NE(h.find("Required,"), std::string::npos)
        << "ANTS-1404: Required enum value missing";
    EXPECT_NE(h.find("Optional,"), std::string::npos)
        << "ANTS-1404: Optional enum value missing";
    EXPECT_NE(h.find("TabSpecific,"), std::string::npos)
        << "ANTS-1404: TabSpecific enum value missing";
    EXPECT_NE(h.find("ProcessGlobal,"), std::string::npos)
        << "ANTS-1404: ProcessGlobal enum value missing";
    EXPECT_NE(h.find("callerCwdContractFor"), std::string::npos)
        << "ANTS-1404: callerCwdContractFor declaration missing";
}
