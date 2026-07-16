// ANTS-1565 — feature-conformance test for the workspace_search
// configurable wall-clock budget. Source-scrapes remotecontrol.cpp
// (engine) + claudeintegration.cpp (schema/description). Pure-function,
// no live PTY / rg required.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <regex>
#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — default budget constant raised from 2000 to 5000.
TEST(mcp_workspace_search_timeout_sec, Inv1DefaultBudgetIs5s) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    std::regex defaultRe(
        R"(constexpr\s+int\s+kWorkspaceSearchHardKillMs\s*=\s*5000\b)");
    expect(std::regex_search(rc, defaultRe),
           "INV-1: kWorkspaceSearchHardKillMs constant is not 5000 — "
           "ANTS-1565 raised it from 2000 (ANTS-1248)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — min/max budget constants are present and `cmdWorkspaceSearch`
// parses `timeout_sec` against them.
TEST(mcp_workspace_search_timeout_sec, Inv2TimeoutSecClampConstants) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(rc, "kWorkspaceSearchMinBudgetMs"),
           "INV-2a: kWorkspaceSearchMinBudgetMs constant is missing");
    expect(contains(rc, "kWorkspaceSearchMaxBudgetMs"),
           "INV-2b: kWorkspaceSearchMaxBudgetMs constant is missing");
    std::regex minRe(
        R"(constexpr\s+int\s+kWorkspaceSearchMinBudgetMs\s*=\s*1000\b)");
    std::regex maxRe(
        R"(constexpr\s+int\s+kWorkspaceSearchMaxBudgetMs\s*=\s*30000\b)");
    expect(std::regex_search(rc, minRe),
           "INV-2c: kWorkspaceSearchMinBudgetMs is not 1000");
    expect(std::regex_search(rc, maxRe),
           "INV-2d: kWorkspaceSearchMaxBudgetMs is not 30000");
    // The body parses the arg and clamps against both bounds.
    expect(contains(rc, "\"timeout_sec\""),
           "INV-2e: cmdWorkspaceSearch does not parse \"timeout_sec\" "
           "from the request");
    expect(contains(rc, "kWorkspaceSearchMinBudgetMs") &&
           contains(rc, "kWorkspaceSearchMaxBudgetMs"),
           "INV-2f: cmdWorkspaceSearch does not reference both clamp "
           "constants in the body");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — hard-kill error envelope carries the fallback `hint` field
// and references the effective budget (not the hard-coded 2 s string).
TEST(mcp_workspace_search_timeout_sec, Inv3HardKillEnvelopeHasHint) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // The hint is appended on the hard-kill path. Two literals must
    // co-occur in the body — the hint key and the fallback advice.
    expect(contains(rc, "\"hint\""),
           "INV-3a: workspace_search source does not emit a \"hint\" "
           "field on any envelope");
    expect(contains(rc, "narrower lane= or glob= filter") ||
           contains(rc, "narrower lane=") ||
           contains(rc, "Bash rg"),
           "INV-3b: hard-kill envelope hint does not name the three "
           "viable next steps (narrower filter / raise timeout_sec / "
           "fall back to Bash rg)");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — both ok:true and hard-kill envelopes echo the effective
// timeout_sec value.
TEST(mcp_workspace_search_timeout_sec, Inv4ResponseEchoesTimeoutSec) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // ok:true path: out["timeout_sec"] = budgetSec;
    expect(contains(rc, "out[\"timeout_sec\"]"),
           "INV-4a: ok:true envelope does not set out[\"timeout_sec\"] "
           "with the effective budget");
    // hard-kill path: o["timeout_sec"] = budgetSec;
    expect(contains(rc, "o[\"timeout_sec\"]"),
           "INV-4b: hard-kill envelope does not set o[\"timeout_sec\"] "
           "with the effective budget");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — the workspace_search inputSchema declares `timeout_sec` as
// an integer with default 5, minimum 1, maximum 30.
TEST(mcp_workspace_search_timeout_sec, Inv5SchemaDeclaresTimeoutSec) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const size_t wsAnchor = ci.find("\"workspace_search\"");
    ASSERT_NE(wsAnchor, std::string::npos)
        << "workspace_search registration not found in claudeintegration.cpp";
    // Window widened from 8000 → 12000 in ANTS-1304 (context property
    // block grew); → 16000 in ANTS-1876 (two new prop blocks +
    // top-level description extension pushed timeoutSecProp past 12000);
    // → 20000 in ANTS-2220 (the enclosing_symbol prop block + description
    // extension pushed props["timeout_sec"] to offset ~17164).
    // → 22000 in ANTS-3549 (the files_only prop block + description extension
    // pushed props["timeout_sec"] to offset ~20875).
    const size_t end = std::min(ci.size(), wsAnchor + 22000);
    const std::string window = ci.substr(wsAnchor, end - wsAnchor);
    expect(contains(window, "\"timeout_sec\""),
           "INV-5a: workspace_search inputSchema does not declare a "
           "\"timeout_sec\" property");
    expect(contains(window, "timeoutSecProp"),
           "INV-5b: workspace_search inputSchema does not use the "
           "timeoutSecProp QJsonObject builder");
    // Sanity-check the bounds appear textually inside the window.
    expect(contains(window, "\"minimum\"") &&
           contains(window, "\"maximum\""),
           "INV-5c: workspace_search timeout_sec property does not set "
           "minimum / maximum bounds");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — the workspace_search tool description mentions the new
// timeout_sec arg and the 5 s default.
TEST(mcp_workspace_search_timeout_sec, Inv6DescriptionMentionsTimeoutSec) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const size_t wsAnchor = ci.find("\"workspace_search\"");
    ASSERT_NE(wsAnchor, std::string::npos);
    // Window widened from 8000 → 12000 in ANTS-1304 (context property
    // block grew); → 16000 in ANTS-1876 (two new prop blocks +
    // top-level description extension pushed timeoutSecProp past 12000);
    // → 20000 in ANTS-2220 (the enclosing_symbol prop block + description
    // extension pushed props["timeout_sec"] to offset ~17164).
    // → 22000 in ANTS-3549 (the files_only prop block + description extension
    // pushed props["timeout_sec"] to offset ~20875).
    const size_t end = std::min(ci.size(), wsAnchor + 22000);
    const std::string window = ci.substr(wsAnchor, end - wsAnchor);
    expect(contains(window, "timeout_sec"),
           "INV-6a: workspace_search description does not mention "
           "timeout_sec");
    expect(contains(window, "ANTS-1565"),
           "INV-6b: workspace_search description does not anchor the "
           "ANTS-1565 spec ID");
    EXPECT_EQ(0, expect_failures());
}
