// ANTS-1402 — feature-conformance test for the dispatch-observer
// unification. Source-scrape pattern.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

size_t countOccurrences(const std::string &hay,
                        const std::string &needle) {
    size_t n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

}  // namespace

// INV-1 — recordDispatch declared in claudeintegration.h.
TEST(mcp_record_dispatch_unification, Inv1DeclarationPresent) {
    expect_reset();
    const std::string h = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    expect(contains(h, "recordDispatch"),
           "INV-1: recordDispatch declared in claudeintegration.h");
    expect(contains(h, "ANTS-1402"),
           "INV-1: ANTS-1402 anchor comment present in header");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — body gates recordCall on result == "ok".
TEST(mcp_record_dispatch_unification, Inv2BodyGatesRecordCall) {
    expect_reset();
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = cc.find(
        "ClaudeIntegration::recordDispatch");
    ASSERT_NE(pos, std::string::npos)
        << "INV-2 precondition: recordDispatch definition missing "
           "from claudeintegration.cpp";
    // ANTS-3681 — the function body, brace-matched, not a byte window.
    const std::string region =
        ants_test::slurpFunctionBody(cc, "ClaudeIntegration::recordDispatch");
    expect(contains(region, "m_tokenUsage.recordCall"),
           "INV-2: recordDispatch forwards to m_tokenUsage.recordCall");
    expect(contains(region, "recordMcpTrace"),
           "INV-2: recordDispatch forwards to recordMcpTrace");
    expect(contains(region, "QLatin1String(\"ok\")") ||
           contains(region, "== \"ok\""),
           "INV-2: recordCall gated on result == \"ok\"");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — dispatch success branch emits one recordDispatch and
// no standalone m_tokenUsage.recordCall under toolHandled.
TEST(mcp_record_dispatch_unification, Inv3SuccessBranchSingleHook) {
    expect_reset();
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cc, "ANTS-1402-INV-3"),
           "INV-3 anchor comment present at dispatch success site");
    // Anchor the count to the toolHandled true-branch — locate
    // it and confirm m_tokenUsage.recordCall appears only inside
    // recordDispatch's own body, not at the dispatch site.
    const auto dispatchPos = cc.find("ANTS-1402-INV-3");
    ASSERT_NE(dispatchPos, std::string::npos);
    // 2000 bytes from the anchor should cover the rest of the
    // toolHandled true-branch (success-path observer block).
    const std::string region = cc.substr(dispatchPos, 2000);
    expect(!contains(region, "m_tokenUsage.recordCall("),
           "INV-3: success branch must NOT call recordCall "
           "directly — must route through recordDispatch");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — failure branch emits one recordDispatch with the
// "tool_not_found" literal.
TEST(mcp_record_dispatch_unification, Inv4FailureBranchUsesHook) {
    expect_reset();
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cc, "ANTS-1402-INV-4"),
           "INV-4 anchor comment present at dispatch failure site");
    const auto pos = cc.find("ANTS-1402-INV-4");
    ASSERT_NE(pos, std::string::npos);
    const std::string region = cc.substr(pos, 800);
    expect(contains(region, "recordDispatch"),
           "INV-4: failure branch routes through recordDispatch");
    expect(contains(region, "\"tool_not_found\""),
           "INV-4: failure branch passes \"tool_not_found\" "
           "as the result string");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — global guard: across the dispatch handler, the
// m_tokenUsage.recordCall string appears ONLY inside
// recordDispatch's body (one call site total in the cpp file).
TEST(mcp_record_dispatch_unification, Inv5RecordCallSingleSiteInCpp) {
    expect_reset();
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const size_t n = countOccurrences(cc, "m_tokenUsage.recordCall(");
    expect(n == 1,
           ("INV-5: m_tokenUsage.recordCall( must appear exactly "
            "once in claudeintegration.cpp (inside recordDispatch). "
            "Found: " + std::to_string(n)).c_str());
    EXPECT_EQ(0, expect_failures());
}
