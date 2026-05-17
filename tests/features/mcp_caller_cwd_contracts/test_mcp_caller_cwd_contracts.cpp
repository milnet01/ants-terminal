// Feature-conformance test for ANTS-1404 Phase 3a — per-tool
// caller_cwd contract enforcement.
// See tests/features/mcp_caller_cwd_contracts/spec.md.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
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

// CLS-1 — Required group classification.
TEST(McpCallerCwdContracts, RequiredToolsClassified) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    // Locate the helper definition.
    const auto pos = cc.find("callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(pos, std::string::npos)
        << "callerCwdContractFor helper missing";
    const auto end = cc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = cc.substr(pos, end - pos);

    for (const auto *tool : {
            "\"get_git_status\"",
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
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    // Find the tools/call body.
    const auto pos = cc.find("else if (method == \"tools/call\")");
    ASSERT_NE(pos, std::string::npos)
        << "tools/call dispatch branch missing";
    // Take a generous window (the dispatch block is several hundred lines).
    const std::string region = cc.substr(pos, 9000);
    EXPECT_NE(region.find("callerCwdContractFor(toolName)"),
              std::string::npos)
        << "ANTS-1404 DISP-1: dispatcher must consult "
           "callerCwdContractFor before the cache + provider path";
}

// DISP-2 — refusal envelope shape.
TEST(McpCallerCwdContracts, RefusalEnvelopeShape) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("else if (method == \"tools/call\")");
    ASSERT_NE(pos, std::string::npos);
    const std::string region = cc.substr(pos, 9000);
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
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("else if (method == \"tools/call\")");
    ASSERT_NE(pos, std::string::npos);
    const std::string region = cc.substr(pos, 9000);
    // The cacheable assignment must depend on !toolHandled so a
    // refusal-set toolHandled value short-circuits the cache lookup.
    EXPECT_NE(region.find("!toolHandled && isIdempotentReadTool(toolName)"),
              std::string::npos)
        << "ANTS-1404 INV-7: cacheable must be gated on "
           "!toolHandled so refusals bypass the cache";
}

// DISP-4 — provider-dispatch guard widened to !toolHandled.
TEST(McpCallerCwdContracts, ProviderDispatchGuardedByToolHandled) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("else if (method == \"tools/call\")");
    ASSERT_NE(pos, std::string::npos);
    const std::string region = cc.substr(pos, 9000);
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
    const std::string h = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
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
