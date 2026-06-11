// Feature-conformance test for ANTS-1283 — session_memory MCP wiring.
// See tests/features/mcp_session_memory/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {


// Region: `// ANTS-1283` session-memory registration block end.
size_t sessionMemoryBlockEnd(const std::string &ci, size_t start) {
    return ci.find("tools.append(t);", start);
}

}  // namespace

// REG-1
TEST(McpSessionMemory, ToolNameRegisteredWithAnchor) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1283");
    ASSERT_NE(pos, std::string::npos)
        << "// ANTS-1283 anchor missing from claudeintegration.cpp";
    const auto end = sessionMemoryBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);
    EXPECT_NE(region.find("t[\"name\"] = \"session_memory\""),
              std::string::npos)
        << "session_memory registration missing under ANTS-1283 anchor";
}

// REG-2
TEST(McpSessionMemory, SchemaRequiredArrayMatchesInv9) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1283");
    ASSERT_NE(pos, std::string::npos);
    const auto end = sessionMemoryBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);

    // Only `op` is schema-required (INV-9).
    EXPECT_NE(region.find("req.append(QStringLiteral(\"op\"))"),
              std::string::npos)
        << "session_memory should schema-require \"op\"";
    // key / value are NOT schema-required (handler enforces).
    EXPECT_EQ(region.find("req.append(QStringLiteral(\"key\"))"),
              std::string::npos)
        << "key should NOT be schema-required (handler enforces)";
    EXPECT_EQ(region.find("req.append(QStringLiteral(\"value\"))"),
              std::string::npos)
        << "value should NOT be schema-required (handler enforces)";
}

// REG-3 — ANTS-1336 amendment. The legacy `cwd` arg is now refused
// entirely — caller_cwd via RcGate is the only project-scope source
// across every op. Pre-ANTS-1336 this test asserted `cwd` WAS
// extracted; flipping the assertion is the regression-lock that the
// cross-project tenancy bypass stays closed.
TEST(McpSessionMemory, CmdExtractsAllArgs) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find(
        "QJsonDocument RemoteControl::cmdSessionMemory");
    ASSERT_NE(pos, std::string::npos)
        << "cmdSessionMemory body missing";
    // Scan to the next top-level closing brace.
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);

    EXPECT_NE(body.find("req.value(QStringLiteral(\"op\"))"),
              std::string::npos)
        << "op arg not extracted";
    EXPECT_NE(body.find("req.value(QStringLiteral(\"key\"))"),
              std::string::npos)
        << "key arg not extracted";
    EXPECT_NE(body.find("req.value(QStringLiteral(\"value\"))"),
              std::string::npos)
        << "value arg not extracted";
    EXPECT_EQ(body.find("req.value(QStringLiteral(\"cwd\"))"),
              std::string::npos)
        << "ANTS-1336: legacy `cwd` arg must NOT be extracted — "
           "caller_cwd via RcGate is the only project-scope source. "
           "Re-introducing the arg re-opens the cross-tenant bypass "
           "(lane-5 HI-1, indie-review 2026-05-14).";
}

// REG-3b (ANTS-1336 / ANTS-1435) — RcGate covers ONLY write ops
// (set, delete). Read ops (get, list) anchor to caller_cwd directly
// without focused-tab match — the storage at sha256(cwd).json is
// per-cwd-hashed and the caller's bucket is self-scoped.
//
// Historical context: ANTS-1336 originally routed EVERY op through
// RcGate to close a read-side tenancy leak. Vestige CC feedback
// 2026-05-16 surfaced that the resulting focused-tab requirement was
// overly strict for legitimate cross-tab reads. ANTS-1435 split the
// gate by op — writes keep the strong guarantee, reads honour
// caller_cwd.
//
// Regression lock-in: confirm the asymmetric routing is in place.
// The read-branch substring must appear BEFORE the first
// RcGate::checkCallerCwd call (i.e. the gate is INSIDE an else
// branch keyed off the read-op condition).
TEST(McpSessionMemory, RcGateOnWriteOpsOnly) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find(
        "QJsonDocument RemoteControl::cmdSessionMemory");
    ASSERT_NE(pos, std::string::npos);
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);

    // Gate is still present (writes still need it).
    EXPECT_NE(body.find("RcGate::checkCallerCwd"), std::string::npos)
        << "ANTS-1435: gate must remain present for write ops";

    // Read-branch sentinel: isReadOp branch decides whether to
    // gate or not. Must appear inside cmdSessionMemory.
    EXPECT_NE(body.find("isReadOp"), std::string::npos)
        << "ANTS-1435: isReadOp branch missing — read/write "
           "asymmetry not in place";

    // The isReadOp condition must appear BEFORE the first
    // RcGate::checkCallerCwd call (the gate is in the else branch).
    const auto readOpPos  = body.find("isReadOp");
    const auto rcGatePos  = body.find("RcGate::checkCallerCwd");
    EXPECT_LT(readOpPos, rcGatePos)
        << "ANTS-1435: isReadOp must precede RcGate::checkCallerCwd "
           "(read branch handles caller_cwd directly; write branch "
           "delegates to gate)";

    // isDir() check on the read branch — INV-4b prevents /etc/passwd
    // from hashing to a real bucket file.
    EXPECT_NE(body.find("isDir()"), std::string::npos)
        << "ANTS-1435 INV-4b: read branch must isDir-check caller_cwd";
}

// REG-4
TEST(McpSessionMemory, ErrorCodesComplete) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find(
        "QJsonDocument RemoteControl::cmdSessionMemory");
    ASSERT_NE(pos, std::string::npos);
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);

    EXPECT_NE(body.find("\"bad_op\""), std::string::npos)
        << "bad_op error code missing";
    EXPECT_NE(body.find("\"bad_key\""), std::string::npos)
        << "bad_key error code missing";
    EXPECT_NE(body.find("\"bad_value\""), std::string::npos)
        << "bad_value error code missing";
    // ANTS-1336: no_project is no longer a literal in this handler
    // — it bubbles up through gate.errorCode (RcGate) and r.code
    // (SessionMemoryEngine). Asserting it inline would be a false
    // requirement post-fix. Coverage moved to the runtime test that
    // exercises an empty-focused-tab scenario.
}

// REG-5
TEST(McpSessionMemory, ProviderLambdaRegisteredInMainWindow) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"session_memory\""),
              std::string::npos)
        << "session_memory provider not registered in mainwindow.cpp";
    // ANTS-1782 — the provider is a pure RC-delegate shim, registered
    // via the rcDelegate(&RemoteControl::cmd*) factory which forwards
    // `args` wholesale. Assert the verb reference rather than the old
    // inline `cmdSessionMemory(args)` call shape.
    EXPECT_NE(mw.find("cmdSessionMemory"), std::string::npos)
        << "cmdSessionMemory not forwarded from provider registration";
}

// REG-6
TEST(McpSessionMemory, HandlerDelegatesToEngine) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find(
        "QJsonDocument RemoteControl::cmdSessionMemory");
    ASSERT_NE(pos, std::string::npos);
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);

    EXPECT_NE(body.find("SessionMemoryEngine::execute"), std::string::npos)
        << "handler should delegate to SessionMemoryEngine::execute";
    EXPECT_NE(body.find("SessionMemoryEngine::parseOp"), std::string::npos)
        << "handler should call parseOp to validate op";
}
