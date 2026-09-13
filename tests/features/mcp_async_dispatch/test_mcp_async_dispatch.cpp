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
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "claudeintegration.h"
#include "guithread.h"
#include "remotecontrol.h"

namespace {

// One tools/call request. The ANTS-1520 fall-through default is Required, so
// an unlisted probe name is classified Required and the dispatcher refuses it
// without a caller_cwd. Supply one.
QByteArray toolsCall(const QString &verb, const QString &callerCwd) {
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
    return QJsonDocument(req).toJson(QJsonDocument::Compact);
}

// Drive one tools/call over the MCP socket, pumping the GUI thread's event
// loop while waiting. Returns the raw reply, or empty on timeout.
QByteArray callVerb(const QString &sockPath, const QString &verb,
                    const QString &callerCwd, int timeoutMs = 8000) {
    QLocalSocket client;
    client.connectToServer(sockPath);
    if (!client.waitForConnected(2000)) return {};

    client.write(toolsCall(verb, callerCwd));
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

// Send one remote-control request, pumping the GUI thread's event loop while
// waiting. Returns the raw reply line, or empty on timeout.
QByteArray callRemote(const QString &sockPath, const QJsonObject &req,
                      int timeoutMs = 8000) {
    QLocalSocket client;
    client.connectToServer(sockPath);
    if (!client.waitForConnected(2000)) return {};
    client.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
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

QJsonObject gitStateRequest(const QString &callerCwd) {
    QJsonObject req;
    req["cmd"]        = QStringLiteral("git-state");
    req["caller_cwd"] = callerCwd;
    return req;
}

// Bind a bare RemoteControl, with no MainWindow, at `sockPath`. start() takes
// its path from ANTS_REMOTE_SOCKET, so set it for the call and put it back.
bool startRemoteControlAt(RemoteControl &rc, const QString &sockPath) {
    const QByteArray previous = qgetenv("ANTS_REMOTE_SOCKET");
    qputenv("ANTS_REMOTE_SOCKET", sockPath.toLocal8Bit());
    const bool ok = rc.start();
    if (previous.isEmpty())
        qunsetenv("ANTS_REMOTE_SOCKET");
    else
        qputenv("ANTS_REMOTE_SOCKET", previous);
    return ok;
}

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

// INV-12 — with a poster installed, a socket route whose MCP twin runs off the
// GUI thread runs on the dispatch worker, and the GUI thread keeps processing
// events while it runs. The poster's wrapper records the job's thread and
// sleeps, so a route still dispatched inline never reaches it and no tick
// lands.
TEST(McpAsyncDispatch, Inv12SocketWorkerRouteRunsOnTheDispatchWorker) {
    // Declared before the harness: a job still queued when the test ends runs
    // inside ~ClaudeIntegration's join and must not touch a destroyed local.
    std::atomic<QThread *> jobRanOn{nullptr};
    std::atomic<int> ticks{0};
    QTemporaryDir rcDir;
    ASSERT_TRUE(rcDir.isValid());
    RemoteControl rc(nullptr);
    Harness h;
    ASSERT_TRUE(h.dir.isValid());

    rc.setDispatchWorkerPoster([&h, &jobRanOn](std::function<void()> job) {
        return h.ci.postWorkerJob([&jobRanOn, job = std::move(job)]() {
            jobRanOn.store(QThread::currentThread());
            QThread::msleep(200);
            job();
        });
    });
    const QString sockPath = rcDir.path() + QStringLiteral("/rc.sock");
    ASSERT_TRUE(startRemoteControlAt(rc, sockPath))
        << "remote-control socket did not bind";

    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, [&ticks]() { ++ticks; });
    heartbeat.start();
    const QByteArray reply = callRemote(sockPath, gitStateRequest(h.dir.path()));
    heartbeat.stop();

    ASSERT_FALSE(reply.isEmpty()) << "no reply from the remote-control socket";
    ASSERT_NE(jobRanOn.load(), nullptr)
        << "git-state ran inline on the GUI thread: the poster was never called";
    EXPECT_NE(jobRanOn.load(), QThread::currentThread())
        << "the socket route's job ran on the GUI thread";
    EXPECT_GT(ticks.load(), 5)
        << "the GUI thread was not processing events while the socket route "
           "ran (ticks=" << ticks.load() << ")";
}

// INV-14 — a socket request to a worker route that finds the shared queue full
// is refused at once with dispatch_queue_full. One held MCP call plus direct
// postWorkerJob calls fill the cap, so the socket and MCP provably share it.
TEST(McpAsyncDispatch, Inv14SocketWorkerRouteRefusedWhenQueueFull) {
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::atomic<int> fillersRan{0};
    QTemporaryDir rcDir;
    ASSERT_TRUE(rcDir.isValid());
    RemoteControl rc(nullptr);
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());
    // Every exit path lets the held job finish, or ~ClaudeIntegration's join
    // would wait for it forever.
    const auto releaseOnExit = qScopeGuard([&release]() { release.store(true); });

    h.ci.registerToolProvider(
        QStringLiteral("ants_hold_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{
            [&holding, &release](const QJsonObject &) -> QString {
                holding.store(true);
                while (!release.load()) QThread::msleep(5);
                return QStringLiteral("{\"ok\":true}");
            }});
    rc.setDispatchWorkerPoster([&h](std::function<void()> job) {
        return h.ci.postWorkerJob(std::move(job));
    });
    const QString sockPath = rcDir.path() + QStringLiteral("/rc.sock");
    ASSERT_TRUE(startRemoteControlAt(rc, sockPath));

    // Job 1: an MCP call that holds the worker.
    QLocalSocket mcp;
    mcp.connectToServer(h.sockPath);
    ASSERT_TRUE(mcp.waitForConnected(2000));
    mcp.write(toolsCall(QStringLiteral("ants_hold_probe"), h.dir.path()));
    mcp.flush();
    QElapsedTimer clock;
    clock.start();
    while (!holding.load() && clock.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    ASSERT_TRUE(holding.load()) << "setup: the held MCP call never started";

    // Jobs 2..64: the rest of the cap, which counts the executing job.
    for (int i = 1; i < 64; ++i) {
        ASSERT_TRUE(h.ci.postWorkerJob([&fillersRan]() { ++fillersRan; }))
            << "setup: job " << i + 1 << " was refused below the cap";
    }

    // The worker is still held here, so a reply at all proves the refusal did
    // not wait for it.
    const QByteArray reply =
        callRemote(sockPath, gitStateRequest(h.dir.path()), 3000);
    EXPECT_TRUE(reply.contains("\"dispatch_queue_full\""))
        << "expected an immediate dispatch_queue_full refusal while the worker "
           "is held; got: "
        << (reply.isEmpty() ? QByteArray("<no reply>") : reply).constData();

    release.store(true);
    clock.restart();
    while (fillersRan.load() < 63 && clock.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    EXPECT_EQ(fillersRan.load(), 63) << "a job accepted below the cap never ran";
}

// INV-15 — a DeferredToolHandler's first reply writes exactly one JSON-RPC
// reply through finishToolDispatch, and the GUI thread is not blocked before
// it. The handler replies twice from a timer. The reply-line count cannot show
// the second, because the first disconnects, but a second recordDispatch adds
// a second trace entry.
TEST(McpAsyncDispatch, Inv15DeferredReplyIsWrittenOnceWithoutBlocking) {
    std::atomic<int> ticks{0};
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());

    h.ci.registerToolProvider(
        QStringLiteral("ants_deferred_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::DeferredToolHandler{
            [](const QJsonObject &, std::function<void(QString)> reply) {
                QTimer::singleShot(200, [reply]() {
                    reply(QStringLiteral("{\"ok\":true,\"reply\":\"first\"}"));
                    reply(QStringLiteral("{\"ok\":true,\"reply\":\"second\"}"));
                });
            }});
    const int tracesBefore = h.ci.mcpTraceSizeForTest();

    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, [&ticks]() { ++ticks; });
    heartbeat.start();
    const QByteArray reply = callVerb(h.sockPath,
                                      QStringLiteral("ants_deferred_probe"),
                                      h.dir.path());
    heartbeat.stop();
    // Give an unlatched second reply time to reach recordDispatch.
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 200)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    ASSERT_FALSE(reply.isEmpty()) << "no reply from the deferred verb";
    EXPECT_TRUE(reply.contains("first")) << reply.constData();
    EXPECT_FALSE(reply.contains("second")) << reply.constData();
    EXPECT_EQ(h.ci.mcpTraceSizeForTest(), tracesBefore + 1)
        << "reply is not latched: a second call reached recordDispatch";
    EXPECT_GT(ticks.load(), 5)
        << "the GUI thread was blocked before the deferred reply (ticks="
        << ticks.load() << ")";
}

// INV-17 — destroying one ClaudeIntegration refuses onGuiThread marshals from
// its own worker only. File → New Window builds a second MainWindow with its
// own ClaudeIntegration and deletes it on close; the first window's
// off-thread verbs must still be served afterwards (ANTS-5142).
TEST(McpAsyncDispatch, Inv17DestroyingOneInstanceRefusesOnlyItsOwnWorker) {
    const auto registerMarshalProbe = [](ClaudeIntegration &ci) {
        ci.registerToolProvider(
            QStringLiteral("ants_marshal_probe"),
            ClaudeIntegration::CallerCwdContract::Required,
            ClaudeIntegration::RcHandler{[](const QJsonObject &) -> QString {
                const std::optional<int> v = ants::onGuiThread([]() { return 1; });
                return v ? QStringLiteral("{\"ok\":true,\"marshal\":\"served-4217\"}")
                         : QStringLiteral("{\"ok\":false,\"code\":\"marshal_refused\"}");
            }});
    };

    Harness first;
    ASSERT_TRUE(first.dir.isValid());
    ASSERT_TRUE(first.start());
    registerMarshalProbe(first.ci);
    auto second = std::make_unique<Harness>();
    ASSERT_TRUE(second->dir.isValid());
    ASSERT_TRUE(second->start());
    registerMarshalProbe(second->ci);

    // Start the second instance's worker, so its teardown has one to refuse.
    ASSERT_TRUE(callVerb(second->sockPath, QStringLiteral("ants_marshal_probe"),
                         second->dir.path()).contains("served-4217"))
        << "setup: the second instance did not serve a marshal";
    ASSERT_TRUE(callVerb(first.sockPath, QStringLiteral("ants_marshal_probe"),
                         first.dir.path()).contains("served-4217"))
        << "setup: the first instance refused a marshal before anything was "
           "destroyed";

    second.reset();

    const QByteArray after = callVerb(first.sockPath,
                                      QStringLiteral("ants_marshal_probe"),
                                      first.dir.path());
    EXPECT_TRUE(after.contains("served-4217"))
        << "destroying the second ClaudeIntegration refused the first's "
           "marshals; got: " << after.constData();
}

// INV-18 (ANTS-5072) — for a call whose handler ran on the dispatch worker, the
// reply transforms run there too; a bare ToolHandler verb runs them on the GUI
// thread. The seam is reset before each call, so a transform that never went
// through transformReply leaves it null and fails the equality.
TEST(McpAsyncDispatch, Inv18ReplyTransformsRunWhereTheHandlerRan) {
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());

    std::atomic<QThread *> handlerThread{nullptr};
    h.ci.registerToolProvider(
        QStringLiteral("ants_async_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[&handlerThread](const QJsonObject &) -> QString {
            handlerThread.store(QThread::currentThread());
            return QStringLiteral("{\"ok\":true}");
        }});
    h.ci.registerToolProvider(
        QStringLiteral("ants_inline_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::ToolHandler{[](const QJsonObject &) -> QString {
            return QStringLiteral("{\"ok\":true}");
        }});

    ClaudeIntegration::resetReplyTransformThreadForTest();
    ASSERT_FALSE(callVerb(h.sockPath, QStringLiteral("ants_async_probe"),
                          h.dir.path()).isEmpty());
    ASSERT_NE(handlerThread.load(), nullptr) << "the handler never ran";
    ASSERT_NE(handlerThread.load(), QThread::currentThread())
        << "setup: the RcHandler verb ran on the GUI thread";
    EXPECT_EQ(ClaudeIntegration::lastReplyTransformThreadForTest(),
              handlerThread.load())
        << "the reply transforms of an off-thread verb did not run on the "
           "worker that ran its handler";

    ClaudeIntegration::resetReplyTransformThreadForTest();
    ASSERT_FALSE(callVerb(h.sockPath, QStringLiteral("ants_inline_probe"),
                          h.dir.path()).isEmpty());
    EXPECT_EQ(ClaudeIntegration::lastReplyTransformThreadForTest(),
              QThread::currentThread())
        << "a bare ToolHandler verb's reply transforms did not run on the GUI "
           "thread";
}

// INV-18 (ANTS-5072) — moving the transforms onto the worker keeps the
// ignored-args advisory. Its keys read m_toolParamKeys, which only tools/list
// fills, so the dispatcher computes them on the GUI thread and hands them over
// in the context. The probe takes a listed verb's name so the list declares
// its parameters.
TEST(McpAsyncDispatch, Inv18OffThreadReplyKeepsTheIgnoredArgsAdvisory) {
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());

    std::atomic<QThread *> handlerThread{nullptr};
    h.ci.registerToolProvider(
        QStringLiteral("git_state"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[&handlerThread](const QJsonObject &) -> QString {
            handlerThread.store(QThread::currentThread());
            return QStringLiteral("{\"ok\":true}");
        }});

    QJsonObject list;
    list["jsonrpc"] = "2.0";
    list["id"]      = 1;
    list["method"]  = "tools/list";
    ASSERT_TRUE(callRemote(h.sockPath, list).contains("\"git_state\""))
        << "setup: tools/list does not declare git_state";

    QJsonObject args;
    args["caller_cwd"]     = h.dir.path();
    args["ants_bogus_arg"] = 1;
    QJsonObject params;
    params["name"]      = "git_state";
    params["arguments"] = args;
    QJsonObject call;
    call["jsonrpc"] = "2.0";
    call["id"]      = 2;
    call["method"]  = "tools/call";
    call["params"]  = params;
    const QByteArray reply = callRemote(h.sockPath, call);

    ASSERT_NE(handlerThread.load(), nullptr) << "the handler never ran";
    ASSERT_NE(handlerThread.load(), QThread::currentThread())
        << "setup: the RcHandler verb ran on the GUI thread";
    EXPECT_TRUE(reply.contains("ignored_args") && reply.contains("ants_bogus_arg"))
        << "an off-thread verb's reply lost the ignored_args advisory; got: "
        << reply.constData();
}

// INV-14 (ANTS-5090) — a dispatch_queue_full refusal is not stored in the
// idempotent-read cache. The probe takes last_audit_summary's name, a cached
// verb, so a stored refusal would answer the call made after the queue drains.
TEST(McpAsyncDispatch, Inv14QueueFullRefusalIsNotCached) {
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::atomic<int> fillersRan{0};
    std::atomic<int> summaryRan{0};
    Harness h;
    ASSERT_TRUE(h.dir.isValid());
    ASSERT_TRUE(h.start());
    // Every exit path lets the held job finish, or ~ClaudeIntegration's join
    // would wait for it forever.
    const auto releaseOnExit = qScopeGuard([&release]() { release.store(true); });

    h.ci.registerToolProvider(
        QStringLiteral("ants_hold_probe"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{
            [&holding, &release](const QJsonObject &) -> QString {
                holding.store(true);
                while (!release.load()) QThread::msleep(5);
                return QStringLiteral("{\"ok\":true}");
            }});
    h.ci.registerToolProvider(
        QStringLiteral("last_audit_summary"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::RcHandler{[&summaryRan](const QJsonObject &) -> QString {
            ++summaryRan;
            return QStringLiteral("{\"ok\":true,\"summary\":\"fresh\"}");
        }});

    QLocalSocket mcp;
    mcp.connectToServer(h.sockPath);
    ASSERT_TRUE(mcp.waitForConnected(2000));
    mcp.write(toolsCall(QStringLiteral("ants_hold_probe"), h.dir.path()));
    mcp.flush();
    QElapsedTimer clock;
    clock.start();
    while (!holding.load() && clock.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    ASSERT_TRUE(holding.load()) << "setup: the held MCP call never started";
    for (int i = 1; i < 64; ++i) {
        ASSERT_TRUE(h.ci.postWorkerJob([&fillersRan]() { ++fillersRan; }))
            << "setup: job " << i + 1 << " was refused below the cap";
    }

    const QByteArray refused = callVerb(
        h.sockPath, QStringLiteral("last_audit_summary"), h.dir.path(), 3000);
    ASSERT_TRUE(refused.contains("dispatch_queue_full"))
        << "setup: expected a dispatch_queue_full refusal; got: "
        << refused.constData();

    release.store(true);
    clock.restart();
    while (fillersRan.load() < 63 && clock.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    ASSERT_EQ(fillersRan.load(), 63) << "setup: the queue never drained";

    const QByteArray served = callVerb(
        h.sockPath, QStringLiteral("last_audit_summary"), h.dir.path());
    EXPECT_EQ(summaryRan.load(), 1)
        << "the call after the queue drained was answered from the cache; got: "
        << served.constData();
    EXPECT_FALSE(served.contains("dispatch_queue_full"))
        << "the cached dispatch_queue_full refusal was served again";
}
