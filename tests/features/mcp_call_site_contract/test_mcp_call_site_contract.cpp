// ANTS-1419 — feature-conformance test for the call-site
// CallerCwdContract declaration. Source-scrape pattern; no
// dispatcher / RemoteControl instantiation needed.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

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

// Pull every registered tool name + the literal text between the
// name-string and the handler argument. The handler is either an
// inline lambda capture-list `[` or, for the ANTS-1782 pure RC-delegate
// shims, an `rcDelegate(` factory call. The captured slice is the place
// where the CallerCwdContract argument must appear, in BOTH forms.
struct Registration {
    std::string name;
    std::string betweenNameAndLambda;  // raw text slice (contract lives here)
};
std::vector<Registration> registrations(const std::string &mw) {
    std::vector<Registration> out;
    static const std::regex rx(
        R"RX(registerToolProvider\("([^"]+)",([^;]*?)(?:\[|rcDelegate\())RX");
    auto it = std::sregex_iterator(mw.begin(), mw.end(), rx);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        out.push_back({(*it)[1].str(), (*it)[2].str()});
    }
    return out;
}

}  // namespace

// INV-1 — registerToolProvider takes a CallerCwdContract.
TEST(mcp_call_site_contract, Inv1HeaderSignature) {
    expect_reset();
    const std::string h = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    expect(contains(h, "registerToolProvider(const QString &name,"),
           "INV-1: registerToolProvider header signature missing");
    expect(contains(h, "CallerCwdContract contract"),
           "INV-1: registerToolProvider must accept "
           "CallerCwdContract as a positional argument "
           "(ANTS-1419)");
    expect(contains(h, "ToolHandler handler"),
           "INV-1: registerToolProvider must accept ToolHandler "
           "as a positional argument");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — every call site passes a contract.
TEST(mcp_call_site_contract, Inv2EveryCallSitePassesContract) {
    expect_reset();
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    const auto regs = registrations(mw);
    expect(!regs.empty(),
           "INV-2: registrations regex returned 0 hits — "
           "mainwindow lost its registerToolProvider calls");
    // Count guard (ANTS-1782) — both handler forms (inline lambda +
    // rcDelegate factory) must be captured. A silent regex under-match
    // would let the per-call contract check go vacuous for whole
    // swathes of tools; pin a floor well below the current ~58 so a
    // genuine regression trips but routine additions don't.
    expect(regs.size() > 50,
           "INV-2: expected > 50 registerToolProvider call sites "
           "(inline + rcDelegate forms) — regex may have drifted");

    std::vector<std::string> missing;
    for (const auto &r : regs) {
        if (!contains(r.betweenNameAndLambda,
                      "CallerCwdContract::")) {
            missing.push_back(r.name);
        }
    }
    if (!missing.empty()) {
        std::fprintf(stderr,
                     "INV-2: %zu registerToolProvider calls miss "
                     "the CallerCwdContract positional argument:\n",
                     missing.size());
        for (const auto &n : missing) {
            std::fprintf(stderr, "  - %s\n", n.c_str());
        }
        std::fprintf(stderr,
                     "Add ClaudeIntegration::CallerCwdContract::"
                     "<Required|Optional|TabSpecific|ProcessGlobal> "
                     "as the 2nd arg, before the handler lambda.\n");
    }
    expect(missing.empty(),
           "INV-2: every registerToolProvider call must pass "
           "an explicit CallerCwdContract (ANTS-1419)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — drift assertion present in the registrar body.
TEST(mcp_call_site_contract, Inv3DriftAssertionPresent) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find(
        "void ClaudeIntegration::registerToolProvider(");
    ASSERT_NE(pos, std::string::npos)
        << "INV-3 precondition: registerToolProvider definition "
           "missing from claudeintegration.cpp";
    // Window past the signature + drift assertion.
    const std::string region = ci.substr(pos, 2000);
    expect(contains(region, "callerCwdContractFor(name)"),
           "INV-3: registerToolProvider must compare the passed "
           "contract against callerCwdContractFor (drift check)");
    expect(contains(region, "ANTS-1419"),
           "INV-3: ANTS-1419 anchor comment present at drift "
           "assertion site");
    expect(contains(region, "Q_ASSERT_X"),
           "INV-3: drift mismatch must trip Q_ASSERT_X so debug "
           "builds catch the silent miscofiguration");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — value type carries the contract.
TEST(mcp_call_site_contract, Inv4MapValueTypeCarriesContract) {
    expect_reset();
    const std::string h = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    expect(contains(h, "RegisteredTool") ||
           contains(h, "std::pair<ToolHandler, CallerCwdContract>"),
           "INV-4: m_toolProviders value type must bundle the "
           "handler and contract (RegisteredTool struct or "
           "equivalent pair)");
    // Look for the contract field declaration inside the struct.
    expect(contains(h, "CallerCwdContract contract;") ||
           contains(h, "CallerCwdContract  contract;"),
           "INV-4: RegisteredTool must carry a CallerCwdContract "
           "field");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — dispatcher prefers the stored contract.
TEST(mcp_call_site_contract, Inv5DispatcherUsesStoredContract) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // The dispatcher's contract-check region. Locate by anchor.
    const auto pos = ci.find(
        "per-tool caller_cwd contract check");
    ASSERT_NE(pos, std::string::npos)
        << "INV-5 precondition: dispatcher's contract-check "
           "anchor missing";
    const std::string region = ci.substr(pos, 2000);
    expect(contains(region, "m_toolProviders.find(toolName)"),
           "INV-5: dispatcher must look up the stored contract "
           "via m_toolProviders before falling back to the "
           "static table");
    expect(contains(region, "it->second.contract"),
           "INV-5: dispatcher must read .contract from the "
           "RegisteredTool entry");
    expect(contains(region, "callerCwdContractFor(toolName)"),
           "INV-5: dispatcher must fall back to "
           "callerCwdContractFor for inline-dispatched tools "
           "(get_session_info, tool_info)");
    EXPECT_EQ(0, expect_failures());
}
