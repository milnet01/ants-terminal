// ANTS-1416 — feature-conformance test that locks the
// CallerCwdContract::Required group for security-critical MCP
// tools. Functional (calls the static helper directly).

#include "../../_support/expect.h"
#include "claudeintegration.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

ANTS_TEST_SCOPE();

namespace {

using C = ClaudeIntegration::CallerCwdContract;

const char *contractName(C c) {
    switch (c) {
        case C::Required:      return "Required";
        case C::Optional:      return "Optional";
        case C::TabSpecific:   return "TabSpecific";
        case C::ProcessGlobal: return "ProcessGlobal";
    }
    return "?";
}

}  // namespace

// INV-1 — session_memory is classified Required.
TEST(mcp_required_contract_baseline, Inv1SessionMemoryRequired) {
    const auto c = ClaudeIntegration::callerCwdContractFor(
        QStringLiteral("session_memory"));
    EXPECT_EQ(c, C::Required)
        << "session_memory must remain Required (ANTS-1336/1435 "
           "+ ANTS-1416). Got: " << contractName(c);
}

// INV-2 — security-critical Required baseline.
TEST(mcp_required_contract_baseline, Inv2RequiredBaseline) {
    const QStringList expectedRequired = {
        QStringLiteral("get_git_status"),
        QStringLiteral("last_audit_summary"),
        QStringLiteral("git_state"),
        QStringLiteral("verify_changes"),
        QStringLiteral("audit_run"),
        QStringLiteral("project_layout"),
        QStringLiteral("session_memory"),
        QStringLiteral("roadmap_log"),
    };
    for (const QString &name : expectedRequired) {
        const auto c = ClaudeIntegration::callerCwdContractFor(name);
        EXPECT_EQ(c, C::Required)
            << name.toStdString()
            << " must be Required (ANTS-1404 baseline)."
               " Got: " << contractName(c);
    }
}

// INV-3 — caller_cwd_info is Optional (not Required) — the verb
// takes caller_cwd as INPUT, not as an anchor.
TEST(mcp_required_contract_baseline, Inv3CallerCwdInfoOptional) {
    const auto c = ClaudeIntegration::callerCwdContractFor(
        QStringLiteral("caller_cwd_info"));
    EXPECT_EQ(c, C::Optional)
        << "caller_cwd_info must stay Optional (ANTS-1400) — "
           "the verb takes caller_cwd as input, not as an anchor. "
           "Got: " << contractName(c);
}
