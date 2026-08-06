// ANTS-1427 — feature-conformance test for the MCP dispatch
// debug-log audit trail. Source-scrape pattern; the log emission
// itself is gated behind DebugLog::Claude (a process-global bit
// not safe to flip from a feature-bundle test), so we lock the
// log-line shapes via source-scrape and rely on the integration
// path being exercised by every other MCP test.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — registerToolProvider wraps every handler with a
// lambda-entry log line. Single-site change so all MCP tools
// inherit the audit trail automatically.
TEST(mcp_dispatch_debug_log, Inv1LambdaEntryWrapper) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find(
        "ClaudeIntegration::registerToolProvider(");
    ASSERT_NE(pos, std::string::npos)
        << "INV-1 precondition: registerToolProvider definition "
           "missing from claudeintegration.cpp";
    // ANTS-2064 — brace-matched body instead of a fixed window that had
    // to be bumped twice (1000 → 2000 → 2600) as the body grew.
    const std::string region = ants_test::slurpFunctionBody(
        ci, "ClaudeIntegration::registerToolProvider(");
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
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find(
        "ClaudeIntegration::recordDispatch(");
    ASSERT_NE(pos, std::string::npos)
        << "INV-2 precondition: recordDispatch definition missing";
    // ANTS-2064 — brace-matched body, not a fixed 1500-char window.
    const std::string region = ants_test::slurpFunctionBody(
        ci, "ClaudeIntegration::recordDispatch(");
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
    const std::string rc = ants_test::slurpRemoteControl();
    const auto pos = rc.find(
        "RemoteControl::cmdTokenUsage(const QJsonObject &req,");
    ASSERT_NE(pos, std::string::npos)
        << "INV-3 precondition: cmdTokenUsage definition missing";
    // ANTS-2064 — brace-matched body, not a fixed 1500-char window.
    const std::string region = ants_test::slurpFunctionBody(
        rc, "RemoteControl::cmdTokenUsage(const QJsonObject &req,");
    expect(contains(region, "ANTS-1427"),
           "INV-3: ANTS-1427 anchor comment present at cmd-enter");
    expect(contains(region, "ANTS_LOG(DebugLog::Claude,"),
           "INV-3: cmdTokenUsage emits ANTS_LOG against Claude");
    expect(contains(region, "mcp cmd-enter cmdTokenUsage ci=%p"),
           "INV-3: cmd-enter log line shape locked, includes "
           "ci pointer for cross-hop comparison");
    EXPECT_EQ(0, expect_failures());
}
