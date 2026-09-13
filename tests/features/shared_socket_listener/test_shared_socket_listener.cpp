// ANTS-5144 — one socket listener per path, shared by every window.
//
// Behavioural for INV-1 to INV-6: bare ClaudeIntegration and RemoteControl
// instances bind sockets in a throwaway directory and real clients drive them.
// INV-7 is a scrape, because it is a claim about where code sits.
//
// See docs/specs/ANTS-5144-shared-socket-listener.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTemporaryDir>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../_support/srcgrep.h"
#include "claudeintegration.h"
#include "localsockethub.h"
#include "remotecontrol.h"

#ifndef ANTS_SOURCE_DIR
#  error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

bool waitUntil(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}

void pump(int ms) {
    waitUntil([] { return false; }, ms);
}

QJsonObject rpcRequest(const QString &method, const QJsonObject &params = {}) {
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 1;
    req["method"]  = method;
    req["params"]  = params;
    return req;
}

QJsonObject toolsCall(const QString &verb, const QString &callerCwd) {
    QJsonObject args;
    args["caller_cwd"] = callerCwd;
    QJsonObject params;
    params["name"]      = verb;
    params["arguments"] = args;
    return rpcRequest(QStringLiteral("tools/call"), params);
}

// One request over the MCP socket; the reply line, or empty on timeout.
QByteArray rpc(const QString &sockPath, const QJsonObject &req,
               int timeoutMs = 5000) {
    QLocalSocket client;
    client.connectToServer(sockPath);
    if (!client.waitForConnected(2000)) return {};
    client.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    client.flush();
    QByteArray reply;
    waitUntil([&] {
        reply += client.readAll();
        return reply.endsWith('\n');
    }, timeoutMs);
    return reply;
}

bool initializeAnswers(const QString &sockPath) {
    return rpc(sockPath, rpcRequest(QStringLiteral("initialize")))
        .contains("\"result\"");
}

ino_t inodeOf(const QString &path) {
    struct stat st{};
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_ino : 0;
}

// A verb every owner registers under one name, answering with its own label.
const QString kLabelVerb = QStringLiteral("ants_owner_label");

void registerLabel(ClaudeIntegration &ci, const QString &label) {
    const QString body = QStringLiteral("{\"label\":\"%1\"}").arg(label);
    ci.registerToolProvider(
        kLabelVerb, ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::ToolHandler{[body](const QJsonObject &) { return body; }});
}

QByteArray callLabel(const QString &sockPath, const QString &callerCwd) {
    return rpc(sockPath, toolsCall(kLabelVerb, callerCwd));
}

// Bind a bare RemoteControl at `sockPath`. start() reads ANTS_REMOTE_SOCKET,
// so set it for the call and put it back.
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

// A socket file with nothing listening on it: bound, then closed unlinked.
bool bindWithoutListening(const QString &path) {
    const QByteArray bytes = QFile::encodeName(path);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (bytes.size() >= int(sizeof(addr.sun_path))) return false;
    std::memcpy(addr.sun_path, bytes.constData(), size_t(bytes.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const bool bound =
        ::bind(fd, static_cast<sockaddr *>(static_cast<void *>(&addr)),
               sizeof(addr)) == 0;
    ::close(fd);
    return bound;
}

// One hook event, delivered the way Claude Code does: write, then disconnect.
bool sendHook(const QString &sockPath, const QString &sessionId) {
    QLocalSocket client;
    client.connectToServer(sockPath);
    if (!client.waitForConnected(2000)) return false;
    QJsonObject event;
    event["hook_event_name"] = QStringLiteral("Notification");
    event["session_id"]      = sessionId;
    client.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
    client.flush();
    if (!waitUntil([&] { return client.bytesToWrite() == 0; }, 2000)) return false;
    client.disconnectFromServer();
    return true;
}

}  // namespace

// INV-1 — a second owner starting on a path the process already serves binds
// nothing and removes no file.
TEST(SharedSocketListener, Inv1SecondOwnerBindsNothing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/mcp.sock");
    ClaudeIntegration a;
    ClaudeIntegration b;

    ASSERT_TRUE(a.startMcpServer(path));
    const ino_t before = inodeOf(path);
    ASSERT_NE(before, ino_t(0));
    ASSERT_TRUE(b.startMcpServer(path));

    EXPECT_EQ(inodeOf(path), before)
        << "the second start removed the socket file and bound a new one";
    EXPECT_TRUE(initializeAnswers(path)) << "no reply to initialize";
}

// INV-2 — while one owner stays attached, destroying any other leaves the path
// accepting.
TEST(SharedSocketListener, Inv2ServerOutlivesAnyOneOwner) {
    for (const bool destroyFirst : {true, false}) {
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/mcp.sock");
        auto a = std::make_unique<ClaudeIntegration>();
        auto b = std::make_unique<ClaudeIntegration>();
        ASSERT_TRUE(a->startMcpServer(path));
        ASSERT_TRUE(b->startMcpServer(path));

        (destroyFirst ? a : b).reset();
        pump(20);

        EXPECT_TRUE(initializeAnswers(path))
            << "destroying the " << (destroyFirst ? "first" : "newest")
            << " owner stopped the path answering";
    }
}

// INV-3 — a path a live server accepts on is never unlinked; a socket file
// nobody accepts on is replaced.
TEST(SharedSocketListener, Inv3LivePathIsNeverUnlinked) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    {
        const QString path = dir.path() + QStringLiteral("/live.sock");
        QLocalServer plain;
        ASSERT_TRUE(plain.listen(path));
        RemoteControl rc(nullptr);

        EXPECT_FALSE(startRemoteControlAt(rc, path))
            << "start took the path from a live server";

        // Drop whatever the start itself connected, so only the client below
        // can leave a pending connection.
        pump(20);
        while (plain.hasPendingConnections())
            delete plain.nextPendingConnection();

        QLocalSocket client;
        client.connectToServer(path);
        ASSERT_TRUE(client.waitForConnected(1000)) << "nothing accepts on the path";
        EXPECT_TRUE(waitUntil([&] { return plain.hasPendingConnections(); }, 1000))
            << "the plain server no longer receives connections on its path";
    }

    {
        const QString path = dir.path() + QStringLiteral("/stale.sock");
        ASSERT_TRUE(bindWithoutListening(path));
        RemoteControl rc(nullptr);

        EXPECT_TRUE(startRemoteControlAt(rc, path))
            << "a socket file nobody accepts on was not replaced";
        QLocalSocket client;
        client.connectToServer(path);
        EXPECT_TRUE(client.waitForConnected(1000))
            << "RemoteControl does not accept on the replaced path";
    }
}

// INV-4 — a connection is served by the most recently activated visible owner,
// else the most recently activated owner; attaching counts as an activation.
TEST(SharedSocketListener, Inv4ConnectionServedByMostRecentlyActivatedVisibleOwner) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/mcp.sock");
    bool alphaVisible = true;
    bool bravoVisible = true;

    ClaudeIntegration alpha;
    ClaudeIntegration bravo;
    registerLabel(alpha, QStringLiteral("owner-alpha"));
    registerLabel(bravo, QStringLiteral("owner-bravo"));
    alpha.setWindowVisibleProbe([&alphaVisible] { return alphaVisible; });
    bravo.setWindowVisibleProbe([&bravoVisible] { return bravoVisible; });
    ASSERT_TRUE(alpha.startMcpServer(path));
    ASSERT_TRUE(bravo.startMcpServer(path));

    ants::LocalSocketHub &hub = ants::LocalSocketHub::instance();
    hub.noteActivated(&bravo);
    hub.noteActivated(&alpha);
    EXPECT_TRUE(callLabel(path, dir.path()).contains("owner-alpha"))
        << "the most recently activated owner did not serve the call";

    alphaVisible = false;
    EXPECT_TRUE(callLabel(path, dir.path()).contains("owner-bravo"))
        << "an invisible owner served while a visible one was attached";

    ClaudeIntegration charlie;
    registerLabel(charlie, QStringLiteral("owner-charlie"));
    ASSERT_TRUE(charlie.startMcpServer(path));
    EXPECT_TRUE(callLabel(path, dir.path()).contains("owner-charlie"))
        << "a newly attached owner ranked behind owners activated earlier";
}

// INV-5 — a hook event reaches processHookEvent on exactly one owner: the one
// whose session probe claims it, else the connection target.
TEST(SharedSocketListener, Inv5HookEventReachesOneOwner) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/hooks.sock");

    ClaudeIntegration a;
    ClaudeIntegration b;
    a.setSessionOwnerProbe([](const QString &) { return false; });
    b.setSessionOwnerProbe([](const QString &s) { return s == QLatin1String("S1"); });
    ASSERT_TRUE(a.startHookServer(path));
    ASSERT_TRUE(b.startHookServer(path));
    ants::LocalSocketHub &hub = ants::LocalSocketHub::instance();
    hub.noteActivated(&b);
    hub.noteActivated(&a);

    ASSERT_TRUE(sendHook(path, QStringLiteral("S1")));
    ASSERT_TRUE(waitUntil([&] {
        return a.hookEventsProcessedForTest() + b.hookEventsProcessedForTest() >= 1;
    }, 3000)) << "the S1 event reached no owner";
    pump(50);
    EXPECT_EQ(b.hookEventsProcessedForTest(), 1) << "the claiming owner missed S1";
    EXPECT_EQ(a.hookEventsProcessedForTest(), 0) << "a second owner processed S1";

    ASSERT_TRUE(sendHook(path, QStringLiteral("S2")));
    ASSERT_TRUE(waitUntil([&] {
        return a.hookEventsProcessedForTest() + b.hookEventsProcessedForTest() >= 2;
    }, 3000)) << "the S2 event reached no owner";
    pump(50);
    EXPECT_EQ(a.hookEventsProcessedForTest(), 1)
        << "an unclaimed session did not go to the last activated owner";
    EXPECT_EQ(b.hookEventsProcessedForTest(), 1) << "a second owner processed S2";
}

// INV-6 — destroying an owner closes and deletes the connections it served.
TEST(SharedSocketListener, Inv6DestroyingAnOwnerClosesItsConnections) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/mcp.sock");
    std::function<void(QString)> heldReply;  // never called: the verb never replies

    auto owner = std::make_unique<ClaudeIntegration>();
    owner->registerToolProvider(
        QStringLiteral("ants_never_reply"),
        ClaudeIntegration::CallerCwdContract::Required,
        ClaudeIntegration::DeferredToolHandler{
            [&heldReply](const QJsonObject &, std::function<void(QString)> reply) {
                heldReply = std::move(reply);
            }});
    ASSERT_TRUE(owner->startMcpServer(path));

    QLocalSocket client;
    client.connectToServer(path);
    ASSERT_TRUE(client.waitForConnected(2000));
    client.write(QJsonDocument(toolsCall(QStringLiteral("ants_never_reply"), dir.path()))
                     .toJson(QJsonDocument::Compact));
    client.flush();
    ASSERT_TRUE(waitUntil([&] { return static_cast<bool>(heldReply); }, 3000))
        << "the deferred verb never ran";

    const QList<QLocalSocket *> taken =
        // The name argument is explicit: the options-only overload is Qt 6.3+.
        owner->findChildren<QLocalSocket *>(QString(), Qt::FindDirectChildrenOnly);
    ASSERT_EQ(taken.size(), 1)
        << "the connection the owner is serving is not parented to the owner";
    const QPointer<QLocalSocket> served(taken.first());

    owner.reset();
    heldReply = nullptr;

    EXPECT_TRUE(waitUntil([&] { return served.isNull(); }, 2000))
        << "the served socket outlived its owner";
    EXPECT_TRUE(waitUntil([&] {
        return client.state() == QLocalSocket::UnconnectedState;
    }, 2000)) << "the client never saw a disconnect";
}

// INV-7 — the accept-time checks stay ahead of reading any request, in each
// connection handler, and the hub serves only through those handlers.
TEST(SharedSocketListener, Inv7AcceptChecksStayInTheHandlers) {
    const std::string srcDir = std::string(ANTS_SOURCE_DIR) + "/src/";
    const std::string ci = ants_test::slurpFile(srcDir + "claudeintegration.cpp");
    const std::string rc = ants_test::slurpFile(srcDir + "remotecontrol.cpp");
    const std::string hub = ants_test::slurpFile(srcDir + "localsockethub.cpp");
    ASSERT_FALSE(ci.empty());
    ASSERT_FALSE(rc.empty());
    ASSERT_FALSE(hub.empty());

    const auto checkHandler = [](const std::string &src, const std::string &sig) {
        const std::string body = ants_test::slurpFunctionBody(src, sig);
        ASSERT_FALSE(body.empty()) << sig << " not found";
        const size_t cred = body.find("SO_PEERCRED");
        const size_t idle = body.find("setInterval(5000)");
        const size_t read = body.find("&QLocalSocket::readyRead");
        ASSERT_NE(cred, std::string::npos) << sig << ": no SO_PEERCRED check";
        ASSERT_NE(idle, std::string::npos) << sig << ": no 5 s idle timer";
        ASSERT_NE(read, std::string::npos) << sig << ": no readyRead connection";
        EXPECT_LT(cred, read) << sig << ": SO_PEERCRED follows readyRead";
        EXPECT_LT(idle, read) << sig << ": the idle timer follows readyRead";
    };
    checkHandler(ci, "void ClaudeIntegration::onMcpConnection() {");
    checkHandler(ci, "void ClaudeIntegration::onHookConnection() {");
    checkHandler(rc, "void RemoteControl::onNewConnection() {");

    // Each attach call, from the needle through its closing parenthesis.
    const auto attachCalls = [](const std::string &src) {
        const std::string needle = "LocalSocketHub::instance().attach(";
        std::vector<std::string> calls;
        for (size_t at = src.find(needle); at != std::string::npos;
             at = src.find(needle, at + 1)) {
            int depth = 0;
            size_t i = at + needle.size() - 1;
            for (; i < src.size(); ++i) {
                if (src[i] == '(') ++depth;
                else if (src[i] == ')' && --depth == 0) break;
            }
            calls.push_back(src.substr(at, i - at + 1));
        }
        return calls;
    };
    const auto ciCalls = attachCalls(ants_test::stripComments(ci));
    ASSERT_EQ(ciCalls.size(), 2u) << "expected an MCP and a hook attach call";
    int mcpServe = 0;
    int hookServe = 0;
    for (const std::string &call : ciCalls) {
        if (call.find("onMcpConnection()") != std::string::npos) ++mcpServe;
        if (call.find("onHookConnection()") != std::string::npos) ++hookServe;
    }
    EXPECT_EQ(mcpServe, 1) << "no attach call serves through onMcpConnection";
    EXPECT_EQ(hookServe, 1) << "no attach call serves through onHookConnection";

    const auto rcCalls = attachCalls(ants_test::stripComments(rc));
    ASSERT_EQ(rcCalls.size(), 1u) << "expected one RemoteControl attach call";
    EXPECT_NE(rcCalls.front().find("onNewConnection()"), std::string::npos)
        << "RemoteControl's attach call does not serve through onNewConnection";

    const std::string hubCode = ants_test::stripComments(hub);
    EXPECT_EQ(ants_test::countOccurrences(hubCode, "nextPendingConnection("), 1u)
        << "the hub pulls connections outside its no-owner close path";
    EXPECT_NE(ants_test::slurpFunctionBody(hubCode, "void LocalSocketHub::closeUnowned(")
                  .find("nextPendingConnection("),
              std::string::npos)
        << "the hub's one nextPendingConnection is not in closeUnowned";
}
