// ANTS-1677 INV-6 — the live tools/list reply, for ANTS-4919's split of
// claudeintegration.cpp. See spec.md beside this file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <sstream>
#include <string>

#include "../../_support/srcgrep.h"
#include "claudeintegration.h"

namespace {

QByteArray request(int id, const QString &method) {
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"]      = id;
    req["method"]  = method;
    req["params"]  = QJsonObject();
    return QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
}

// Send one request on its own connection (the server answers one request per
// connection, then closes it) and return the reply line, pumping the event
// loop while waiting. Empty on timeout.
QByteArray roundTrip(const QString &sockPath, const QByteArray &line,
                     int timeoutMs = 10000) {
    QLocalSocket client;
    client.connectToServer(sockPath);
    if (!client.waitForConnected(2000)) return {};
    client.write(line);
    client.flush();
    QByteArray reply;
    QElapsedTimer clock;
    clock.start();
    while (!reply.endsWith('\n') && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        reply += client.readAll();
    }
    return reply;
}

// Statement 2's text side: names on lines matching
// tools/check-readme-claims.sh's MCP-tool pattern.
QSet<QString> sourceToolNames(const std::string &text) {
    static const QRegularExpression kTool(QStringLiteral(
        R"re(^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*\["name"\][[:space:]]*=[[:space:]]*"([a-z_]+)";)re"));
#if defined(ANTS_LUA_PLUGINS)
    const bool luaBuild = true;
#else
    const bool luaBuild = false;
#endif
    QSet<QString> names;
    bool inLuaBlock = false;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        const QString line = QString::fromStdString(raw);
        const QString trimmed = line.trimmed();
        if (trimmed == QLatin1String("#ifdef ANTS_LUA_PLUGINS")) {
            inLuaBlock = true;
            continue;
        }
        if (inLuaBlock && trimmed.startsWith(QLatin1String("#endif"))) {
            inLuaBlock = false;
            continue;
        }
        if (inLuaBlock && !luaBuild) continue;
        const auto m = kTool.match(line);
        if (m.hasMatch()) names.insert(m.captured(1));
    }
    return names;
}

std::string sorted(const QSet<QString> &names) {
    QStringList list(names.cbegin(), names.cend());
    list.sort();
    return list.join(QLatin1Char(' ')).toStdString();
}

}  // namespace

TEST(McpToolsListLive, ReplyMatchesSourceAndDumpsWhenAsked) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString sockPath = dir.path() + QStringLiteral("/mcp.sock");

    ClaudeIntegration ci;
    // The trace descriptor is listed only by a process that registered the
    // verb; a stand-in makes it part of the reply.
    ci.registerToolProvider(QStringLiteral("trace"), ClaudeIntegration::CallerCwdContract::Required,
                            ClaudeIntegration::ToolHandler(
                                [](const QJsonObject &) { return QString(); }));
    ASSERT_TRUE(ci.startMcpServer(sockPath));

    ASSERT_FALSE(roundTrip(sockPath, request(1, QStringLiteral("initialize"))).isEmpty());
    const QByteArray reply = roundTrip(sockPath, request(2, QStringLiteral("tools/list")));
    ASSERT_TRUE(reply.endsWith('\n')) << "no tools/list reply";

    const QByteArray dumpPath = qgetenv("ANTS_TOOLS_LIST_DUMP");
    if (!dumpPath.isEmpty()) {
        QFile out(QString::fromLocal8Bit(dumpPath));
        ASSERT_TRUE(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(out.write(reply), reply.size());
    }

    const QJsonArray tools = QJsonDocument::fromJson(reply)
                                 .object().value("result").toObject()
                                 .value("tools").toArray();
    QSet<QString> live;
    for (const auto &t : tools) live.insert(t.toObject().value("name").toString());

    const QSet<QString> text = sourceToolNames(ants_test::slurpClaudeIntegration());
    ASSERT_FALSE(live.isEmpty());
    ASSERT_FALSE(text.isEmpty());
    EXPECT_EQ(sorted(live - text), "") << "listed live, absent from the source";
    EXPECT_EQ(sorted(text - live), "") << "in the source, not listed live";
}
