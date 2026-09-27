// ANTS-5485 — deprecated MCP verbs are marked, still work, and are counted.
// Contract: spec.md here.

#include "../standalone_mcp_server/mcpd_session.h"

#include "claudeintegration.h"
#include "mcpdeprecation.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using ants_test::McpdSession;

namespace {

const QStringList &deprecatedVerbs() {
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

QList<QJsonObject> recordedLines() {
    QList<QJsonObject> out;
    QFile f(mcp::deprecatedCallsPath());
    if (!f.open(QIODevice::ReadOnly)) return out;
    for (const QByteArray &l : f.readAll().split('\n'))
        if (!l.trimmed().isEmpty()) out << QJsonDocument::fromJson(l).object();
    return out;
}

}  // namespace

// INV-1
TEST(McpVerbDeprecation, Inv1ListedAndMarked) {
    ClaudeIntegration ci;
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", &out);
    const QJsonArray tools = QJsonDocument::fromJson(line).object()
                                 .value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(tools.isEmpty());
    QHash<QString, QString> desc;
    for (const QJsonValue &v : tools)
        desc.insert(v.toObject().value(QStringLiteral("name")).toString(),
                    v.toObject().value(QStringLiteral("description")).toString());

    for (const QString &verb : deprecatedVerbs()) {
        ASSERT_TRUE(desc.contains(verb))
            << verb.toStdString() << " is in the table but not a listed tool";
        EXPECT_TRUE(desc.value(verb).startsWith(QStringLiteral("DEPRECATED (ANTS-5485): use ")))
            << verb.toStdString() << ": " << desc.value(verb).left(120).toStdString();
        EXPECT_FALSE(mcp::deprecatedReplacement(verb).isEmpty());
    }
    EXPECT_FALSE(desc.value(QStringLiteral("git_state")).contains(QStringLiteral("DEPRECATED")))
        << "a verb outside the table carries no deprecation line";
}

// INV-2 + INV-3 — through the built server, as a session meets it.
TEST(McpVerbDeprecation, Inv2CallStillAnswersAndSaysSo) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QFile::remove(mcp::deprecatedCallsPath());
    McpdSession mcpd(tmp.path(), tmp.filePath(QStringLiteral("no-terminal.sock")));
    ASSERT_TRUE(mcpd.started());

    const QJsonObject reply = mcpd.call(QStringLiteral("current_state"),
                                        {{"caller_cwd", tmp.path()}});
    ASSERT_FALSE(reply.isEmpty()) << mcpd.stderrText().toStdString();
    const QJsonObject d = reply.value(QStringLiteral("deprecated")).toObject();
    EXPECT_EQ(d.value(QStringLiteral("replacement")).toString(),
              QStringLiteral("session_orient"))
        << QJsonDocument(reply).toJson().toStdString();
    EXPECT_EQ(d.value(QStringLiteral("tracking")).toString(), QStringLiteral("ANTS-5485"));

    const QList<QJsonObject> lines = recordedLines();
    ASSERT_EQ(lines.size(), 1) << mcp::deprecatedCallsPath().toStdString();
    EXPECT_EQ(lines.first().value(QStringLiteral("verb")).toString(),
              QStringLiteral("current_state"));
    EXPECT_EQ(lines.first().value(QStringLiteral("caller_cwd")).toString(), tmp.path());
    EXPECT_FALSE(lines.first().value(QStringLiteral("at")).toString().isEmpty());
    EXPECT_EQ(QFileInfo(mcp::deprecatedCallsPath()).permissions()
                  & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
              QFileDevice::Permissions())
        << "the log is owner-only";
}

// INV-3 — another verb writes nothing.
TEST(McpVerbDeprecation, Inv3OtherVerbsAreNotRecorded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QFile::remove(mcp::deprecatedCallsPath());
    McpdSession mcpd(tmp.path(), tmp.filePath(QStringLiteral("no-terminal.sock")));
    ASSERT_TRUE(mcpd.started());
    const QJsonObject reply = mcpd.call(QStringLiteral("session_orient"),
                                        {{"caller_cwd", tmp.path()}});
    ASSERT_FALSE(reply.isEmpty()) << mcpd.stderrText().toStdString();
    EXPECT_FALSE(reply.contains(QStringLiteral("deprecated")));
    EXPECT_TRUE(recordedLines().isEmpty());
}
