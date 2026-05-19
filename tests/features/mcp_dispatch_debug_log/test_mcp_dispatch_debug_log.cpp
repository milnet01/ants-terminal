// ANTS-1427 — feature-conformance test for the MCP dispatch
// debug-log audit trail. Source-scrape pattern; the log emission
// itself is gated behind DebugLog::Claude (a process-global bit
// not safe to flip from a feature-bundle test), so we lock the
// log-line shapes via source-scrape and rely on the integration
// path being exercised by every other MCP test.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

// INV-1 — registerToolProvider wraps every handler with a
// lambda-entry log line. Single-site change so all MCP tools
// inherit the audit trail automatically.
TEST(mcp_dispatch_debug_log, Inv1LambdaEntryWrapper) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find(
        "ClaudeIntegration::registerToolProvider(");
    ASSERT_NE(pos, std::string::npos)
        << "INV-1 precondition: registerToolProvider definition "
           "missing from claudeintegration.cpp";
    // 2000-byte window covers the function body. Bumped from 1000
    // after ANTS-1419 inserted the contract-drift assertion ahead
    // of the wrapper lambda.
    const std::string region = ci.substr(pos, 2000);
    expect(contains(region, "ANTS-1427"),
           "INV-1: ANTS-1427 anchor comment present at wrapper site");
    expect(contains(region, "ANTS_LOG(DebugLog::Claude,"),
           "INV-1: wrapper emits ANTS_LOG against the Claude category");
    expect(contains(region, "mcp lambda-enter tool=%s"),
           "INV-1: lambda-entry log line shape locked");
    expect(contains(region, "inner = std::move(handler)"),
           "INV-1: handler is moved into the wrapper "
           "(no copy, no original-handler retention)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — recordDispatch emits the dispatch-end log line. This
// is the unified observation point (ANTS-1402) so logging here
// counts every dispatch exactly once across success + failure.
TEST(mcp_dispatch_debug_log, Inv2RecordDispatchEnd) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find(
        "ClaudeIntegration::recordDispatch(");
    ASSERT_NE(pos, std::string::npos)
        << "INV-2 precondition: recordDispatch definition missing";
    // 1500-byte window covers the (small) function body.
    const std::string region = ci.substr(pos, 1500);
    expect(contains(region, "ANTS-1427"),
           "INV-2: ANTS-1427 anchor comment present at recordDispatch");
    expect(contains(region, "ANTS_LOG(DebugLog::Claude,"),
           "INV-2: recordDispatch emits ANTS_LOG against Claude");
    expect(contains(region, "mcp dispatch tool=%s"),
           "INV-2: dispatch-end log line shape locked");
    expect(contains(region, "arg_b="),
           "INV-2: arg byte count in dispatch-end log");
    expect(contains(region, "out_b="),
           "INV-2: out byte count in dispatch-end log");
    expect(contains(region, "dur_us="),
           "INV-2: duration µs in dispatch-end log");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — cmdTokenUsage emits the middle-checkpoint log. The
// chain becomes lambda-enter → cmd-enter → recordDispatch, which
// matches the user's classic printf-debugging style: pointer
// values at each hop let future debugging spot exactly where a
// variable changes.
TEST(mcp_dispatch_debug_log, Inv3MiddleCheckpointCmdTokenUsage) {
    expect_reset();
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = rc.find(
        "RemoteControl::cmdTokenUsage(const QJsonObject &req,");
    ASSERT_NE(pos, std::string::npos)
        << "INV-3 precondition: cmdTokenUsage definition missing";
    const std::string region = rc.substr(pos, 1500);
    expect(contains(region, "ANTS-1427"),
           "INV-3: ANTS-1427 anchor comment present at cmd-enter");
    expect(contains(region, "ANTS_LOG(DebugLog::Claude,"),
           "INV-3: cmdTokenUsage emits ANTS_LOG against Claude");
    expect(contains(region, "mcp cmd-enter cmdTokenUsage ci=%p"),
           "INV-3: cmd-enter log line shape locked, includes "
           "ci pointer for cross-hop comparison");
    EXPECT_EQ(0, expect_failures());
}
