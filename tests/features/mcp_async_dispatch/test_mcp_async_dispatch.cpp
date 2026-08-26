// ANTS-2132 — the MCP dispatcher runs eligible verbs off the GUI thread.
//
// Behavioural, not a source scrape. Every other test around this change reads
// mainwindow.cpp for a factory spelling, and a scrape cannot tell whether a
// verb ACTUALLY leaves the GUI thread — which is the entire claim. This drives
// a live ClaudeIntegration over a real MCP socket and observes the threads.
//
// See docs/specs/ANTS-2132-async-mcp-dispatch.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>

#include "claudeintegration.h"

namespace {

// Drive one tools/call over the MCP socket, pumping the GUI thread's event
// loop while waiting. Returns the raw reply, or empty on timeout.
QByteArray callVerb(const QString &sockPath, const QString &verb,
                    const QString &callerCwd, int timeoutMs = 8000) {
    QLocalSocket client;
    client.connectToServer(sockPath);
    if (!client.waitForConnected(2000)) return {};

    // The ANTS-1520 fall-through default is Required, so an unlisted probe
    // name is classified Required and the dispatcher refuses it without a
    // caller_cwd. Supply one.
    QJsonObject args;
    args["caller_cwd"] = callerCwd;
    QJsonObject params;
    params["name"]      = verb;
    params["arguments"] = args;
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 1;
    req["method"]  = "tools/call";
    req["params"]  = params;
    client.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    client.flush();

    QByteArray reply;
    QElapsedTimer clock;
    clock.start();
    while (!reply.endsWith('\n') && clock.elapsed() < timeoutMs) {
        // AllEvents so queued cross-thread invocations are delivered — that
        // is how the worker's result gets back here.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        reply += client.readAll();
    }
    return reply;
}

// A ClaudeIntegration with a live MCP socket in a throwaway directory.
struct Harness {
    QTemporaryDir dir;
    ClaudeIntegration ci;
    QString sockPath;

    Harness() {
        sockPath = dir.path() + QStringLiteral("/mcp.sock");
    }
    bool start() { return ci.startMcpServer(sockPath); }
};

}  // namespace

// INV-1 — an eligible verb does not run on the GUI thread, and the GUI thread
// keeps processing events while it runs.
//
// The timer is the discriminator. Before this change the readyRead handler ran
// the verb inline, so nothing else on that thread could run for its whole
// duration and a 10 ms timer could not fire. If the join ever comes back, this
// count collapses toward zero.
TEST(McpAsyncDispatch, Inv1EligibleVerbLeavesTheGuiThreadAndItKeepsPainting) {
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start()) << "MCP server did not bind";

    QThread *const guiThread = QThread::currentThread();
    std::atomic<QThread *> ranOn{nullptr};

    h.ci.registerToolProvider(
        QStringLiteral("ants_async_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[&ranOn](const QJsonObject &) -> QString {
            ranOn.store(QThread::currentThread());
            QThread::msleep(200);
            return QStringLiteral("{\"ok\":true}");
        }});

    std::atomic<int> ticks{0};
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, [&ticks]() { ++ticks; });
    heartbeat.start();

    const QByteArray reply = callVerb(h.sockPath,
                                      QStringLiteral("ants_async_probe"),
                                      h.dir.path());
    heartbeat.stop();

    ASSERT_FALSE(reply.isEmpty()) << "no reply from the dispatcher";
    EXPECT_TRUE(reply.contains("\"result\""))
        << "expected a JSON-RPC result, got: " << reply.constData();

    ASSERT_NE(ranOn.load(), nullptr) << "the handler never ran";
    EXPECT_NE(ranOn.load(), guiThread)
        << "the verb ran on the GUI thread — dispatch is still synchronous";
    // ~20 ticks are available across a 200 ms verb; require a clear margin
    // over the one-or-two a blocked thread could still post.
    EXPECT_GT(ticks.load(), 5)
        << "the GUI thread was not processing events while the verb ran "
           "(ticks=" << ticks.load() << ")";
}

// INV-4 — a TabSpecific verb reads live terminal state through MainWindow and
// must stay on the GUI thread, however it was registered.
TEST(McpAsyncDispatch, Inv4TabSpecificStaysOnTheGuiThread) {
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());

    QThread *const guiThread = QThread::currentThread();
    std::atomic<QThread *> ranOn{nullptr};

    // get_text is TabSpecific in the ANTS-1404 contract table, so the
    // registration must match it or registerToolProvider refuses the drift.
    h.ci.registerToolProvider(
        QStringLiteral("get_text"),
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        ClaudeIntegration::RcHandler{[&ranOn](const QJsonObject &) -> QString {
            ranOn.store(QThread::currentThread());
            return QStringLiteral("{\"ok\":true}");
        }});

    ASSERT_FALSE(callVerb(h.sockPath, QStringLiteral("get_text"),
                             h.dir.path()).isEmpty());
    ASSERT_NE(ranOn.load(), nullptr);
    EXPECT_EQ(ranOn.load(), guiThread)
        << "a TabSpecific verb was dispatched off the GUI thread; it reads "
           "widget state and would be racing the GUI";
}

// INV-5 — a handler registered through the bare ToolHandler overload is never
// dispatched off the GUI thread. That overload is what every hand-written
// inline lambda in mainwindow.cpp uses, and those capture MainWindow.
TEST(McpAsyncDispatch, Inv5BareToolHandlerStaysOnTheGuiThread) {
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());

    QThread *const guiThread = QThread::currentThread();
    std::atomic<QThread *> ranOn{nullptr};

    h.ci.registerToolProvider(
        QStringLiteral("ants_inline_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::ToolHandler{[&ranOn](const QJsonObject &) -> QString {
            ranOn.store(QThread::currentThread());
            return QStringLiteral("{\"ok\":true}");
        }});

    ASSERT_FALSE(
        callVerb(h.sockPath, QStringLiteral("ants_inline_probe"),
                 h.dir.path()).isEmpty());
    ASSERT_NE(ranOn.load(), nullptr);
    EXPECT_EQ(ranOn.load(), guiThread)
        << "an inline-lambda handler was dispatched off the GUI thread";
}

// INV-2 — off-thread verbs execute one at a time. One worker, not one per
// call, so no pair of off-thread verbs begins to overlap.
TEST(McpAsyncDispatch, Inv2OffThreadVerbsDoNotOverlap) {
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());

    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> completed{0};

    h.ci.registerToolProvider(
        QStringLiteral("ants_async_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{
            [&](const QJsonObject &) -> QString {
                const int now = ++concurrent;
                int seen = maxConcurrent.load();
                while (now > seen && !maxConcurrent.compare_exchange_weak(seen, now)) {}
                QThread::msleep(60);
                --concurrent;
                ++completed;
                return QStringLiteral("{\"ok\":true}");
            }});

    // Three clients in flight at once, so the requests genuinely queue.
    QLocalSocket a, b, c;
    QLocalSocket *socks[] = {&a, &b, &c};
    for (QLocalSocket *s : socks) {
        s->connectToServer(h.sockPath);
        ASSERT_TRUE(s->waitForConnected(2000));
        QJsonObject params;
        params["name"]      = QStringLiteral("ants_async_probe");
        QJsonObject cargs;
        cargs["caller_cwd"] = h.dir.path();
        params["arguments"] = cargs;
        QJsonObject req;
        req["jsonrpc"] = "2.0";
        req["id"]      = 1;
        req["method"]  = "tools/call";
        req["params"]  = params;
        s->write(QJsonDocument(req).toJson(QJsonDocument::Compact));
        s->flush();
    }

    QElapsedTimer clock;
    clock.start();
    while (completed.load() < 3 && clock.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    EXPECT_EQ(completed.load(), 3) << "not every queued verb ran";
    EXPECT_EQ(maxConcurrent.load(), 1)
        << "two off-thread verbs overlapped; the dispatcher is not "
           "serialising them onto one worker";
}
