// Feature-conformance test for ANTS-2158 — eager-load the highest-frequency
// MCP verbs so they escape Claude Code's tool-search deferral (callable
// without a ToolSearch hop). The tools/list builder isn't invokable in
// isolation, so this is a source-grep wiring contract over the marking
// pass in claudeintegration.cpp (the shape other wiring tests use).
// See tests/features/mcp_eager_load/spec.md + ROADMAP ANTS-2158.

#include <gtest/gtest.h>
#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include "claudeintegration.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <string>

ANTS_TEST_SCOPE();

namespace {
bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
}  // namespace

TEST(McpEagerLoad, AlwaysLoadMarkingWired) {
    const std::string ci =
        ants_test::slurpFile(std::string(ANTS_SOURCE_DIR)
                             + "/src/claudeintegration.cpp");

    // The exact, version-honoured field name + placement (Claude Code
    // v2.1.121+, code.claude.com/docs/en/mcp.md).
    EXPECT_TRUE(has(ci, "anthropic/alwaysLoad"))
        << "eager-load _meta field missing from tools/list builder";
    EXPECT_TRUE(has(ci, "kEagerVerbs"));

    // The curated set itself, read from its initializer: the grep / Read
    // substitutes are in it, and the rarely called write verbs are not —
    // each member costs context at every session start.
    const auto open = ci.find("kEagerVerbs = {");
    ASSERT_NE(open, std::string::npos);
    const auto close = ci.find("};", open);
    ASSERT_NE(close, std::string::npos);
    const std::string set = ci.substr(open, close - open);
    for (const char *verb : {"\"workspace_search\"", "\"find_definition\"",
                             "\"file_outline\"", "\"read_region\""}) {
        EXPECT_TRUE(has(set, verb)) << "missing eager verb: " << verb;
    }
    for (const char *verb : {"\"roadmap_log\"", "\"changelog_log\""}) {
        EXPECT_FALSE(has(set, verb))
            << "always-loaded but rarely called: " << verb;
    }
}

namespace {
QJsonObject mcpCall(ClaudeIntegration &ci, const QByteArray &request) {
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(request, &out);
    return QJsonDocument::fromJson(line).object()
        .value(QStringLiteral("result")).toObject();
}
}  // namespace

// ANTS-5628 — an always-loaded verb's argument descriptions are one line on
// the wire, and tool_info still serves the full text.
TEST(McpEagerLoad, Ants5628WireArgumentsAreShort) {
    ClaudeIntegration ci;
    const QJsonArray tools =
        mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
            .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(tools.isEmpty());

    constexpr int kMaxArgDescription = 140;
    int eager = 0;
    for (const auto &v : tools) {
        const QJsonObject t = v.toObject();
        if (!t.value(QStringLiteral("_meta")).toObject()
                 .value(QStringLiteral("anthropic/alwaysLoad")).toBool())
            continue;
        ++eager;
        const QString name = t.value(QStringLiteral("name")).toString();
        const QJsonObject props = t.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject();
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const int len = it.value().toObject()
                .value(QStringLiteral("description")).toString().size();
            EXPECT_LE(len, kMaxArgDescription)
                << name.toStdString() << "/" << it.key().toStdString()
                << " is " << len << " chars on the wire";
        }
    }
    EXPECT_EQ(eager, 4) << "the four always-loaded verbs";

    // The full text is one tool_info call away.
    const QJsonObject info = mcpCall(ci,
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tool_info","arguments":{"name":"workspace_search"}}})");
    const QString text = info.value(QStringLiteral("content")).toArray()
        .at(0).toObject().value(QStringLiteral("text")).toString();
    EXPECT_TRUE(text.contains(QStringLiteral("the rg scan still runs")))
        << "tool_info no longer carries count_only's full description";
}
