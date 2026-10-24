// ANTS-1435 — session_memory + project_layout read-op tenancy split.
// Source-scrape against remotecontrol.cpp + claudeintegration.cpp
// for the asymmetric-routing anchors; ReadRefusalsRun runs the read
// refusals. The corresponding rewritten
// behavioural REG-3b (RcGateOnWriteOpsOnly) lives in
// tests/features/mcp_session_memory/test_mcp_session_memory.cpp.

#include "../../_support/expect.h"
#include "remotecontrol.h"
#include "rootprovider.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
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
    expect(contains(body, "canonicalCallerDir(rawCaller"),
           "INV-4b: read branch uses the shared canonicalise+isDir check");
    EXPECT_EQ(0, expect_failures());
}

// INV-4b / INV-5 — the shared check, and every refusal it drives, run
// for real: a missing path and a file both refuse cwd_bad, each verb
// in its own words.
TEST(session_memory_read_caller_cwd, ReadRefusalsRun) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = tmp.filePath(QStringLiteral("nope"));
    const QString file = tmp.filePath(QStringLiteral("f.txt"));
    QFile f(file);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.close();

    ants::ServerCwdRootProvider roots;
    RemoteControl rc(nullptr, nullptr, &roots);
    auto call = [&](const char *verb, const QString &cwd) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = cwd;
        req[QStringLiteral("op")] = QStringLiteral("get");
        req[QStringLiteral("key")] = QStringLiteral("k");
        req[QStringLiteral("skill")] = QStringLiteral("s");
        const std::string v = verb;
        if (v == "session_memory") return rc.cmdSessionMemory(req).object();
        if (v == "workflow_state") return rc.cmdWorkflowState(req).object();
        return rc.cmdProjectLayout(req).object();
    };
    const auto expectBad = [](const QJsonObject &env, const QString &error) {
        EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
                  QStringLiteral("cwd_bad"));
        EXPECT_EQ(env.value(QStringLiteral("error")).toString(), error);
    };

    expectBad(call("session_memory", missing),
              QStringLiteral("session_memory: caller_cwd \"%1\" does not exist")
                  .arg(missing));
    expectBad(call("session_memory", file),
              QStringLiteral("session_memory: caller_cwd \"%1\" is not a directory")
                  .arg(file));
    expectBad(call("workflow_state", file),
              QStringLiteral("workflow_state: caller_cwd unresolvable"));
    expectBad(call("project_layout", file),
              QStringLiteral("project_layout: caller_cwd \"%1\" is not a directory")
                  .arg(file));
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
    expect(contains(body, "canonicalCallerDir(rawCaller"),
           "INV-5: the shared canonicalise+isDir check");
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
