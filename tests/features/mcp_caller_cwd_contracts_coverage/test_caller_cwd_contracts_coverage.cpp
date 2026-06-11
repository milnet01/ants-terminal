// ANTS-1417 feature-conformance test — every registered MCP tool
// must have an explicit CallerCwdContract branch in
// callerCwdContractFor. Source-scrape pattern.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {


// Pull every registered tool name out of mainwindow.cpp by matching
// `registerToolProvider("<name>"`. The lone registration in
// claudeintegration.cpp is the registrar itself; mainwindow holds
// every actual tool registration.
std::set<std::string> registeredToolNames(const std::string &mw) {
    std::set<std::string> out;
    static const std::regex rx(
        R"RX(registerToolProvider\("([^"]+)")RX");
    auto it = std::sregex_iterator(mw.begin(), mw.end(), rx);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

// Pull every explicitly-classified tool name out of the
// callerCwdContractFor() function body in claudeintegration.cpp.
std::set<std::string> classifiedToolNames(const std::string &ci) {
    std::set<std::string> out;
    // Match `if (toolName == QStringLiteral("<name>"))`
    static const std::regex rx(
        R"RX(toolName\s*==\s*QStringLiteral\("([^"]+)"\))RX");
    auto it = std::sregex_iterator(ci.begin(), ci.end(), rx);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

}  // namespace

// INV-1 — every registered tool name has an explicit classification.
TEST(mcp_caller_cwd_contracts_coverage,
     Inv1EveryRegisteredToolClassified) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    const auto registered = registeredToolNames(mw);
    const auto classified = classifiedToolNames(ci);

    expect(!registered.empty(),
           "INV-1: registeredToolNames returned 0 tools — regex "
           "broke or mainwindow lost its registrations");
    expect(!classified.empty(),
           "INV-1: classifiedToolNames returned 0 tools — regex "
           "broke or callerCwdContractFor lost its branches");

    std::vector<std::string> missing;
    for (const auto &name : registered) {
        if (classified.find(name) == classified.end()) {
            missing.push_back(name);
        }
    }

    if (!missing.empty()) {
        std::fprintf(stderr,
                     "INV-1: %zu registered MCP tools have no "
                     "explicit branch in callerCwdContractFor:\n",
                     missing.size());
        for (const auto &n : missing) {
            std::fprintf(stderr, "  - %s\n", n.c_str());
        }
        std::fprintf(stderr,
                     "Each falls to the default Optional classification "
                     "silently. Add an explicit branch in "
                     "src/claudeintegration.cpp::callerCwdContractFor.\n");
    }
    expect(missing.empty(),
           "INV-1: every registered MCP tool must have an explicit "
           "CallerCwdContract entry — see stderr for the list");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 / INV-3 — inline-dispatched tools (get_session_info,
// tool_info) MAY appear in callerCwdContractFor without being in the
// registered set. The asymmetric scope is documented; this test
// just makes sure they're still classified somewhere.
TEST(mcp_caller_cwd_contracts_coverage,
     Inv2InlineDispatchedToolsStillClassified) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto classified = classifiedToolNames(ci);

    expect(classified.find("get_session_info") != classified.end(),
           "INV-2: get_session_info must remain in "
           "callerCwdContractFor (inline-dispatched but classified "
           "for symmetry — ANTS-1404 carve-out)");
    expect(classified.find("tool_info") != classified.end(),
           "INV-3: tool_info must remain in callerCwdContractFor "
           "(inline-dispatched, ANTS-1399)");
    EXPECT_EQ(0, expect_failures());
}
