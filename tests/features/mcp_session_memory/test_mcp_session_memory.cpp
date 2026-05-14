// Feature-conformance test for ANTS-1283 — session_memory MCP wiring.
// See tests/features/mcp_session_memory/spec.md.

#include <gtest/gtest.h>

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

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Region: `// ANTS-1283` session-memory registration block end.
size_t sessionMemoryBlockEnd(const std::string &ci, size_t start) {
    return ci.find("tools.append(t);", start);
}

}  // namespace

// REG-1
TEST(McpSessionMemory, ToolNameRegisteredWithAnchor) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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

// REG-3
TEST(McpSessionMemory, CmdExtractsAllArgs) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
    EXPECT_NE(body.find("req.value(QStringLiteral(\"cwd\"))"),
              std::string::npos)
        << "cwd arg not extracted";
}

// REG-4
TEST(McpSessionMemory, ErrorCodesComplete) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
    EXPECT_NE(body.find("\"no_project\""), std::string::npos)
        << "no_project error code missing";
}

// REG-5
TEST(McpSessionMemory, ProviderLambdaRegisteredInMainWindow) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"session_memory\""),
              std::string::npos)
        << "session_memory provider not registered in mainwindow.cpp";
    EXPECT_NE(mw.find("cmdSessionMemory(args)"), std::string::npos)
        << "cmdSessionMemory not forwarded from provider lambda";
}

// REG-6
TEST(McpSessionMemory, HandlerDelegatesToEngine) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
