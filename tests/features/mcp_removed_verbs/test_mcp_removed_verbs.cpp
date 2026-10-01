// ANTS-5485 — removed MCP verbs say what replaced them.
// Contract: spec.md here.

#include "../../_support/srcgrep.h"

#include "claudeintegration.h"
#include "mcpdeprecation.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_MCP_REGISTRY_SOURCE
#  error "ANTS_MCP_REGISTRY_SOURCE compile definition required"
#endif

namespace {

const QStringList &removedVerbs() {
    static const QStringList v = {
        QStringLiteral("get_git_status"), QStringLiteral("current_state"),
        QStringLiteral("session_brief"), QStringLiteral("cross_doc_diff"),
        QStringLiteral("cold_eyes_cross_doc_diff"),
        QStringLiteral("cold_eyes_single_doc"), QStringLiteral("cold_eyes_fold_in"),
        QStringLiteral("indie_review_orchestrate"),
        QStringLiteral("indie_review_brief"),
        QStringLiteral("indie_review_synthesis_prompt"),
        QStringLiteral("indie_review_fold_in"), QStringLiteral("test_audit_fold_in"),
        QStringLiteral("test_audit_recheck"),
        QStringLiteral("test_audit_synthesis_prompt"),
        QStringLiteral("debt_sweep_scan"), QStringLiteral("debt_sweep_apply_fix"),
        QStringLiteral("debt_sweep_triage_prompt"), QStringLiteral("debt_sweep_defer"),
        QStringLiteral("plan_template"), QStringLiteral("roadmap_branch_drift")};
    return v;
}

}  // namespace

// INV-2
TEST(McpRemovedVerbs, Inv2RemovedVerbErrorShape) {
    for (const QString &verb : removedVerbs()) {
        const QString r = mcp::removedReplacement(verb);
        EXPECT_FALSE(r.isEmpty()) << verb.toStdString();
        const QJsonObject e = mcp::removedVerbError(verb);
        EXPECT_EQ(e.value(QStringLiteral("code")).toInt(), -32602)
            << verb.toStdString();
        EXPECT_EQ(e.value(QStringLiteral("message")).toString(),
                  QStringLiteral("Tool %1 was removed (ANTS-5485); use %2 instead.")
                      .arg(verb, r))
            << verb.toStdString();
        const QJsonObject d = e.value(QStringLiteral("data")).toObject();
        EXPECT_EQ(d.value(QStringLiteral("code")).toString(),
                  QStringLiteral("verb_removed")) << verb.toStdString();
        EXPECT_EQ(d.value(QStringLiteral("replacement")).toString(), r)
            << verb.toStdString();
    }
    EXPECT_EQ(mcp::removedReplacement(QStringLiteral("get_git_status")),
              QStringLiteral("git_state"));
}

// INV-2 — the unknown-tool branch of finishToolDispatch consults the table
// before the generic message.
TEST(McpRemovedVerbs, Inv2DispatchUsesIt) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t generic = src.find("QString(\"Unknown tool: %1\")");
    ASSERT_NE(generic, std::string::npos);
    // The success branch ends with this line; the failure branch follows it.
    const size_t branch = src.rfind("haveResult = true;", generic);
    ASSERT_NE(branch, std::string::npos);
    const std::string between = src.substr(branch, generic - branch);
    EXPECT_NE(between.find("mcp::removedVerbError(toolName)"), std::string::npos)
        << "the unknown-tool branch does not consult the removed-verb table";
}

// INV-3
TEST(McpRemovedVerbs, Inv3OtherNamesUnchanged) {
    EXPECT_TRUE(mcp::removedVerbError(QStringLiteral("no_such_verb")).isEmpty());
    EXPECT_TRUE(mcp::removedReplacement(QStringLiteral("no_such_verb")).isEmpty());
    EXPECT_TRUE(mcp::removedReplacement(QStringLiteral("git_state")).isEmpty());
}

// INV-4
TEST(McpRemovedVerbs, Inv4ToolInfoNamesReplacement) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t msg = src.find("\"tool_info: no MCP tool registered \"");
    ASSERT_NE(msg, std::string::npos);
    const size_t code = src.rfind("QStringLiteral(\"unknown_tool\")", msg);
    ASSERT_NE(code, std::string::npos);
    const size_t end = src.find("} else {", msg);
    ASSERT_NE(end, std::string::npos);
    const std::string branch = src.substr(code, end - code);
    EXPECT_NE(branch.find("mcp::removedReplacement("), std::string::npos)
        << "tool_info's unknown_tool refusal does not name the replacement";
}

// INV-7 — registry half.
TEST(McpRemovedVerbs, Inv7KeptVerbsRegistered) {
    const std::string reg = ants_test::slurpFile(ANTS_MCP_REGISTRY_SOURCE);
    ASSERT_FALSE(reg.empty());
    for (const char *kept : {"indie_review_partition", "indie_review_corroborate",
                             "indie_review_dispatch", "cold_eyes_partition",
                             "cold_eyes_brief", "test_audit_partition",
                             "test_audit_brief", "session_orient",
                             "verify_changes"}) {
        const std::string needle =
            std::string("registerToolProvider(\"") + kept + "\"";
        EXPECT_NE(reg.find(needle), std::string::npos) << kept;
    }
    for (const QString &verb : removedVerbs()) {
        const std::string needle =
            "registerToolProvider(\"" + verb.toStdString() + "\"";
        EXPECT_EQ(reg.find(needle), std::string::npos) << verb.toStdString();
    }
}

TEST(McpRemovedVerbs, Inv7RemovedVerbsNotListed) {
    ClaudeIntegration ci;
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", &out);
    const QJsonArray tools = QJsonDocument::fromJson(line).object()
                                 .value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(tools.isEmpty());
    QSet<QString> names;
    for (const auto &v : tools)
        names.insert(v.toObject().value(QStringLiteral("name")).toString());
    EXPECT_TRUE(names.contains(QStringLiteral("session_orient")));
    for (const QString &verb : removedVerbs())
        EXPECT_FALSE(names.contains(verb)) << verb.toStdString();
}
