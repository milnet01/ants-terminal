// ANTS-1435 — session_memory + project_layout read-op tenancy split.
// Source-scrape against remotecontrol.cpp + claudeintegration.cpp
// for the asymmetric-routing anchors. The corresponding rewritten
// behavioural REG-3b (RcGateOnWriteOpsOnly) lives in
// tests/features/mcp_session_memory/test_mcp_session_memory.cpp.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Slice between two function signatures to confine the search to
// the verb's body. Mirrors the boundedBetween pattern used by the
// ANTS-1429 unrecognised_format test.
std::string bodyBetween(const std::string &cpp,
                        const std::string &startSig,
                        const std::string &endSig) {
    const auto s = cpp.find(startSig);
    if (s == std::string::npos) return {};
    const auto e = cpp.find(endSig, s + startSig.size());
    if (e == std::string::npos) return {};
    return cpp.substr(s, e - s);
}

}  // namespace

// INV-1 — read branch present + anchored to caller_cwd directly.
TEST(session_memory_read_caller_cwd, Inv1ReadBranchAnchorsToCallerCwd) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    const std::string body = bodyBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdSessionMemory",
        "// ---------------------------------------------------------------------------\n"
        "// ANTS-1430");
    ASSERT_FALSE(body.empty())
        << "cmdSessionMemory body not bounded";
    expect(contains(body, "ANTS-1435"),
           "INV-1: ANTS-1435 anchor present");
    expect(contains(body, "gate routing is now ASYMMETRIC"),
           "INV-1: asymmetric-routing anchor present");
    expect(contains(body, "isReadOp"),
           "INV-1: isReadOp branch present");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — write ops keep RcGate; gate call is AFTER the isReadOp
// branch inside the cmdSessionMemory body.
TEST(session_memory_read_caller_cwd, Inv2WriteBranchKeepsRcGate) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    const std::string body = bodyBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdSessionMemory",
        "// ---------------------------------------------------------------------------\n"
        "// ANTS-1430");
    ASSERT_FALSE(body.empty());
    const auto readOpPos = body.find("isReadOp");
    const auto gatePos   = body.find("RcGate::checkCallerCwd");
    expect(readOpPos != std::string::npos,
           "INV-2: isReadOp branch present");
    expect(gatePos != std::string::npos,
           "INV-2: RcGate::checkCallerCwd present");
    expect(readOpPos < gatePos,
           "INV-2: isReadOp branch must precede RcGate call "
           "(gate is in the else branch)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 / INV-4 / INV-4b — read-branch refusal codes.
TEST(session_memory_read_caller_cwd, Inv3_4_4bReadRefusalCodes) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    const std::string body = bodyBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdSessionMemory",
        "// ---------------------------------------------------------------------------\n"
        "// ANTS-1430");
    ASSERT_FALSE(body.empty());
    expect(contains(body, "cwd_missing"),
           "INV-3: cwd_missing literal in read branch");
    expect(contains(body, "cwd_bad"),
           "INV-4: cwd_bad literal in read branch");
    expect(contains(body, "is not a directory"),
           "INV-4b: 'is not a directory' message present");
    expect(contains(body, "QFileInfo(canon).isDir()"),
           "INV-4b: isDir() check on canonicalised path");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — project_layout follows the same canonicalise+isDir path.
TEST(session_memory_read_caller_cwd, Inv5ProjectLayoutSamePattern) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    const std::string body = bodyBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdProjectLayout",
        "\n}\n");
    ASSERT_FALSE(body.empty())
        << "cmdProjectLayout body not bounded";
    expect(contains(body, "ANTS-1404 + ANTS-1435"),
           "INV-5: anchor present in cmdProjectLayout");
    expect(contains(body, "QFileInfo(cwd).isDir()"),
           "INV-5: isDir check on canonicalised cwd");
    expect(!contains(body, "RcGate::checkCallerCwd"),
           "INV-5: RcGate body call removed from cmdProjectLayout");
    expect(contains(body, "cwd_bad"),
           "INV-5: cwd_bad refusal present");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — session_memory registered as Required in the contract
// registry.
TEST(session_memory_read_caller_cwd, Inv6ContractRegistryEntry) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "ANTS-1435 — session_memory"),
           "INV-6: session_memory anchor in callerCwdContractFor");
    expect(contains(cpp,
        "if (toolName == QStringLiteral(\"session_memory\"))     return C::Required;"),
           "INV-6: session_memory classified Required");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — envelope shape parity. session_memory uses smErr (4-field);
// project_layout uses bare envelope. Confirms the per-verb difference
// is preserved.
TEST(session_memory_read_caller_cwd, Inv7EnvelopeShapeParity) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    const std::string smBody = bodyBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdSessionMemory",
        "// ---------------------------------------------------------------------------\n"
        "// ANTS-1430");
    const std::string plBody = bodyBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdProjectLayout",
        "\n}\n");
    ASSERT_FALSE(smBody.empty());
    ASSERT_FALSE(plBody.empty());
    // session_memory uses smErr (4-field shape).
    expect(contains(smBody, "smErr("),
           "INV-7: cmdSessionMemory uses smErr for refusal envelope");
    // project_layout uses bare QJsonObject envelope (no smErr).
    expect(!contains(plBody, "smErr("),
           "INV-7: cmdProjectLayout does NOT use smErr (bare envelope)");
    EXPECT_EQ(0, expect_failures());
}
