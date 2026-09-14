// ANTS-5089 — the MCP socket answers an unparsable request with a JSON-RPC
// error instead of waiting out the idle timer.
//
// Behavioural: a live ClaudeIntegration MCP socket in a throwaway directory,
// driven by a real client. See tests/features/mcp_parse_error_reply/spec.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>

#include "claudeintegration.h"

namespace {

struct Harness {
    QTemporaryDir dir;
    ClaudeIntegration ci;
    QString sockPath;
    Harness() { sockPath = dir.path() + QStringLiteral("/mcp.sock"); }
    bool start() { return ci.startMcpServer(sockPath); }
};

// Send raw bytes and return whatever comes back within timeoutMs, stopping
// early at a complete line.
QByteArray sendRaw(const QString &sockPath, const QByteArray &bytes,
                   int timeoutMs) {
    QLocalSocket client;
    client.connectToServer(sockPath);
    if (!client.waitForConnected(2000)) return QByteArrayLiteral("<no connect>");
    client.write(bytes);
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

QJsonObject replyObject(const QByteArray &reply) {
    return QJsonDocument::fromJson(reply).object();
}

int errorCode(const QByteArray &reply) {
    return replyObject(reply).value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toInt();
}

}  // namespace

// INV-1 — a malformed line gets -32700.
TEST(McpParseErrorReply, Inv1MalformedLineGetsParseError) {
    Harness h;
    ASSERT_TRUE(h.start());
    const QByteArray reply =
        sendRaw(h.sockPath, QByteArrayLiteral("{not json\n"), 3000);
    ASSERT_TRUE(reply.endsWith('\n'))
        << "INV-1: no reply within 3 s: '" << reply.constData() << "'";
    EXPECT_EQ(errorCode(reply), -32700) << reply.constData();
    const QJsonObject o = replyObject(reply);
    EXPECT_EQ(o.value(QStringLiteral("jsonrpc")).toString(),
              QStringLiteral("2.0"));
    EXPECT_TRUE(o.value(QStringLiteral("id")).isNull()) << reply.constData();
}

// INV-2 — a non-object line gets -32600.
TEST(McpParseErrorReply, Inv2NonObjectLineGetsInvalidRequest) {
    Harness h;
    ASSERT_TRUE(h.start());
    const QByteArray reply =
        sendRaw(h.sockPath, QByteArrayLiteral("[1,2]\n"), 3000);
    ASSERT_TRUE(reply.endsWith('\n'))
        << "INV-2: no reply within 3 s: '" << reply.constData() << "'";
    EXPECT_EQ(errorCode(reply), -32600) << reply.constData();
}

// INV-3 — a request in progress gets no reply.
TEST(McpParseErrorReply, Inv3RequestInProgressKeepsWaiting) {
    Harness h;
    ASSERT_TRUE(h.start());
    EXPECT_TRUE(sendRaw(h.sockPath,
                        QByteArrayLiteral("{\"jsonrpc\":\"2.0\""), 600).isEmpty())
        << "INV-3: an unframed partial request must not be answered";
    EXPECT_TRUE(sendRaw(h.sockPath,
                        QByteArrayLiteral("{\n  \"jsonrpc\": \"2.0\",\n"),
                        600).isEmpty())
        << "INV-3: a buffer with an embedded newline is still arriving";
}
