// ANTS-5089 — the Claude hook and MCP sockets cap concurrent connections.
//
// Behavioural: live servers in a throwaway directory, driven by real
// clients. See tests/features/local_socket_connection_cap/spec.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QTemporaryDir>

#include <memory>
#include <vector>

#include "claudeintegration.h"

namespace {

void pump(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

std::unique_ptr<QLocalSocket> connectIdle(const QString &path) {
    auto s = std::make_unique<QLocalSocket>();
    s->connectToServer(path);
    s->waitForConnected(2000);
    return s;
}

// Open the cap's worth of idle clients, then one more. Returns the
// clients; the last is the one past the cap.
std::vector<std::unique_ptr<QLocalSocket>> fillPastCap(const QString &path) {
    std::vector<std::unique_ptr<QLocalSocket>> clients;
    for (int i = 0; i < ClaudeIntegration::kMaxLiveConnections; ++i) {
        clients.push_back(connectIdle(path));
        pump(2);
    }
    pump(100);
    clients.push_back(connectIdle(path));
    pump(1000);
    return clients;
}

bool connected(const QLocalSocket &s) {
    return s.state() == QLocalSocket::ConnectedState;
}

}  // namespace

// INV-1
TEST(LocalSocketConnectionCap, Inv1McpRefusesPastCap) {
    QTemporaryDir dir;
    ClaudeIntegration ci;
    const QString path = dir.path() + QStringLiteral("/mcp.sock");
    ASSERT_TRUE(ci.startMcpServer(path));
    auto clients = fillPastCap(path);
    EXPECT_TRUE(connected(*clients.front())) << "INV-1: an admitted client was dropped";
    EXPECT_FALSE(connected(*clients.back())) << "INV-1: the client past the cap was admitted";
}

// INV-2
TEST(LocalSocketConnectionCap, Inv2HookRefusesPastCap) {
    QTemporaryDir dir;
    ClaudeIntegration ci;
    const QString path = dir.path() + QStringLiteral("/hook.sock");
    ASSERT_TRUE(ci.startHookServer(path));
    auto clients = fillPastCap(path);
    EXPECT_TRUE(connected(*clients.front())) << "INV-2: an admitted client was dropped";
    EXPECT_FALSE(connected(*clients.back())) << "INV-2: the client past the cap was admitted";
}

// INV-3
TEST(LocalSocketConnectionCap, Inv3SlotFreesWhenAConnectionCloses) {
    QTemporaryDir dir;
    ClaudeIntegration ci;
    const QString path = dir.path() + QStringLiteral("/mcp.sock");
    ASSERT_TRUE(ci.startMcpServer(path));
    auto clients = fillPastCap(path);
    ASSERT_FALSE(connected(*clients.back())) << "precondition: INV-1 holds";
    clients.front()->disconnectFromServer();
    pump(300);
    auto late = connectIdle(path);
    pump(1000);
    EXPECT_TRUE(connected(*late)) << "INV-3: a freed slot was not reused";
}
