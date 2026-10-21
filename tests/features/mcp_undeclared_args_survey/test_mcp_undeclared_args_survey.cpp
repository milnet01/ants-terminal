// ANTS-4659 — survey ledger of undeclared MCP arguments.
// Contract: tests/features/mcp_undeclared_args_survey/spec.md.

#include "../../_support/srcgrep.h"

#include "claudeintegration.h"
#include "mcpundeclaredargs.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace UA = McpUndeclaredArgs;

namespace {

QJsonObject parseLine(const QByteArray &line) {
    return QJsonDocument::fromJson(line).object();
}

QList<QByteArray> fileLines(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QList<QByteArray> out;
    for (const QByteArray &l : f.readAll().split('\n'))
        if (!l.isEmpty()) out.append(l);
    return out;
}

// tools/list first: it fills the per-verb schema keys the advisory diffs
// against. Then one tools/call of tool_info with `args`.
void dispatchToolInfo(ClaudeIntegration &ci, const QJsonObject &args) {
    QByteArray reply;
    McpReplyChannel out([&reply](const QByteArray &l) { reply = l; }, [] {},
                        [] { return true; });
    const QJsonObject list{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
    ci.handleMcpLine(QJsonDocument(list).toJson(QJsonDocument::Compact), &out);
    ASSERT_FALSE(reply.isEmpty()) << "tools/list: no reply";
    reply.clear();
    const QJsonObject call{
        {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "tool_info"}, {"arguments", args}}}};
    ci.handleMcpLine(QJsonDocument(call).toJson(QJsonDocument::Compact), &out);
    ASSERT_FALSE(reply.isEmpty()) << "tool_info: no reply";
}

}  // namespace

// INV-1
TEST(McpUndeclaredArgsSurvey, Inv1LineHoldsNamesNotValues) {
    const QJsonObject o = parseLine(UA::recordLine(
        QStringLiteral("2026-10-21T10:00:00Z"), QStringLiteral("roadmap_query"),
        {QStringLiteral("query_text"), QStringLiteral("sort")}));
    EXPECT_EQ(o.value("ts").toString(), QStringLiteral("2026-10-21T10:00:00Z"));
    EXPECT_EQ(o.value("tool").toString(), QStringLiteral("roadmap_query"));
    EXPECT_EQ(o.value("keys").toArray(),
              (QJsonArray{QStringLiteral("query_text"), QStringLiteral("sort")}));
    EXPECT_EQ(o.keys(), (QStringList{"keys", "tool", "ts"}))
        << "a line carries no field beyond these three";
}

// INV-2
TEST(McpUndeclaredArgsSurvey, Inv2NamesAreCapped) {
    QStringList keys;
    for (int i = 0; i < UA::kMaxKeysPerRecord + 3; ++i)
        keys << QStringLiteral("k%1").arg(i);
    keys[0] = QString(UA::kMaxKeyChars + 40, QLatin1Char('x'));
    const QJsonObject o = parseLine(
        UA::recordLine(QStringLiteral("t"), QStringLiteral("v"), keys));
    const QJsonArray kept = o.value("keys").toArray();
    EXPECT_EQ(kept.size(), UA::kMaxKeysPerRecord);
    EXPECT_EQ(kept.at(0).toString().size(), UA::kMaxKeyChars);
    EXPECT_EQ(o.value("keys_dropped").toInt(), 3);
}

// INV-3
TEST(McpUndeclaredArgsSurvey, Inv3CapKeepsNewestAndFileIsPrivate) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("survey.jsonl"));
    constexpr qint64 kSmallCap = 512;
    for (int i = 1; i <= 40; ++i)
        ASSERT_TRUE(UA::appendRecord(path, QStringLiteral("verb%1").arg(i),
                                     {QStringLiteral("k")}, kSmallCap));
    const QList<QByteArray> lines = fileLines(path);
    qint64 total = 0;
    for (const QByteArray &l : lines) total += l.size() + 1;
    EXPECT_LE(total, kSmallCap);
    ASSERT_FALSE(lines.isEmpty());
    EXPECT_EQ(parseLine(lines.last()).value("tool").toString(), QStringLiteral("verb40"));
    EXPECT_NE(parseLine(lines.first()).value("tool").toString(), QStringLiteral("verb1"));
    EXPECT_EQ(QFileInfo(path).permissions() & 0x0077, QFileDevice::Permissions{})
        << "group and other must have no access";
}

// INV-4
TEST(McpUndeclaredArgsSurvey, Inv4DispatchRecordsUndeclaredArgument) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("survey.jsonl"));
    ClaudeIntegration ci;
    ci.setUndeclaredArgsLedgerPath(path);
    dispatchToolInfo(ci, {{"name", "tool_info"}, {"not_a_tool_info_arg", 1}});
    const QList<QByteArray> lines = fileLines(path);
    ASSERT_EQ(lines.size(), 1);
    const QJsonObject o = parseLine(lines.first());
    EXPECT_EQ(o.value("tool").toString(), QStringLiteral("tool_info"));
    EXPECT_EQ(o.value("keys").toArray(), (QJsonArray{"not_a_tool_info_arg"}));
}

// INV-5
TEST(McpUndeclaredArgsSurvey, Inv5NoPathOrNothingUndeclaredWritesNothing) {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("survey.jsonl"));
    {
        ClaudeIntegration ci;   // no path set
        dispatchToolInfo(ci, {{"name", "tool_info"}, {"not_a_tool_info_arg", 1}});
    }
    {
        ClaudeIntegration ci;
        ci.setUndeclaredArgsLedgerPath(path);
        dispatchToolInfo(ci, {{"name", "tool_info"}});
    }
    EXPECT_FALSE(QFileInfo::exists(path));
}

// INV-6
TEST(McpUndeclaredArgsSurvey, Inv6BothProcessesSetThePath) {
    const std::string mcpd = ants_test::slurpFile(ANTS_SOURCE_DIR "/src/mcpdmain.cpp");
    const size_t callReturn = mcpd.find("if (callRequest.isCall) return");
    const size_t setPath = mcpd.find("setUndeclaredArgsLedgerPath(");
    ASSERT_NE(callReturn, std::string::npos);
    ASSERT_NE(setPath, std::string::npos);
    EXPECT_GT(setPath, callReturn) << "a --call run must not write the user's file";

    const std::string mw = ants_test::slurpFile(ANTS_SOURCE_DIR "/src/mainwindow_mcp.cpp");
    EXPECT_NE(mw.find("setUndeclaredArgsLedgerPath("), std::string::npos)
        << "the terminal finishes forwarded calls, so it must record too";
}
