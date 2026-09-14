// ANTS-5093 — a remote-control reply nobody reads does not pin the socket.
//
// INV-1 and INV-2 drive RemoteControl::armReplyDrainGuard on a bare
// QLocalServer connection with a raw AF_UNIX client, so the client's read
// pace is under the test's control (a QLocalSocket client would drain the
// kernel buffer into its own whenever events run). INV-3 is a scrape.
//
// See tests/features/rc_reply_drain_guard/spec.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../_support/srcgrep.h"
#include "remotecontrol.h"

namespace {

bool waitUntil(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}

// A raw client: connected, and reading only when the test says so.
int rawConnect(const QString &path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray p = path.toLocal8Bit();
    if (p.size() >= int(sizeof(addr.sun_path))) { ::close(fd); return -1; }
    std::memcpy(addr.sun_path, p.constData(), size_t(p.size()));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// The server end of one connection, with a reply already written to it.
struct Pair {
    QTemporaryDir tmp;
    QLocalServer server;
    int clientFd = -1;
    QPointer<QLocalSocket> peer;

    bool open(const QByteArray &reply) {
        if (!tmp.isValid()) return false;
        const QString path = tmp.path() + QStringLiteral("/s");
        if (!server.listen(path)) return false;
        clientFd = rawConnect(path);
        if (clientFd < 0) return false;
        if (!waitUntil([this] { return server.hasPendingConnections(); }, 3000))
            return false;
        peer = server.nextPendingConnection();
        if (!peer) return false;
        peer->write(reply);
        peer->flush();
        peer->disconnectFromServer();
        return true;
    }
    ~Pair() {
        if (clientFd >= 0) ::close(clientFd);
    }
};

bool closed(const QPointer<QLocalSocket> &s) {
    return !s || s->state() == QLocalSocket::UnconnectedState;
}

}  // namespace

// INV-1 — a stalled reader is aborted.
TEST(RcReplyDrainGuard, Inv1StalledReaderIsAborted) {
    Pair pair;
    // Far larger than the kernel socket buffer, so the write cannot finish.
    ASSERT_TRUE(pair.open(QByteArray(8 * 1024 * 1024, 'x')));
    ASSERT_FALSE(closed(pair.peer))
        << "setup: the reply must still be pending when the guard is armed";

    RemoteControl::armReplyDrainGuard(pair.peer, 300);

    EXPECT_TRUE(waitUntil([&] { return closed(pair.peer); }, 5000))
        << "INV-1: a reply nobody reads must not keep the socket open";
}

// INV-2 — a draining reader is not cut.
TEST(RcReplyDrainGuard, Inv2DrainingReaderGetsEveryByte) {
    constexpr qsizetype kReply = 2 * 1024 * 1024;
    Pair pair;
    ASSERT_TRUE(pair.open(QByteArray(kReply, 'y')));

    RemoteControl::armReplyDrainGuard(pair.peer, 1000);

    std::atomic<qsizetype> received{0};
    std::atomic<bool> eof{false};
    std::thread reader([&] {
        char buf[64 * 1024];
        for (;;) {
            const ssize_t n = ::read(pair.clientFd, buf, sizeof(buf));
            if (n <= 0) break;
            received += n;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        eof = true;
    });

    const bool finished = waitUntil([&] { return eof.load(); }, 20000);
    if (!finished) ::shutdown(pair.clientFd, SHUT_RDWR);
    reader.join();

    EXPECT_TRUE(finished) << "INV-2: the reader never saw end of stream";
    EXPECT_EQ(received.load(), kReply)
        << "INV-2: a reader that keeps reading must receive the whole reply";
}

// INV-3 — the reply write arms the guard.
TEST(RcReplyDrainGuard, Inv3ReplyWriteArmsTheGuard) {
    // Every RemoteControl TU joined (ANTS-3833), so the handler may move.
    const std::string src = ants_test::slurpRemoteControl();
    ASSERT_FALSE(src.empty());
    const std::string body =
        ants_test::slurpFunctionBody(src, "void RemoteControl::onNewConnection(");
    ASSERT_FALSE(body.empty());
    const auto disc = body.find("sock->disconnectFromServer();");
    const auto arm = body.find(
        "RemoteControl::armReplyDrainGuard(sock.data(), kReplyDrainIdleMs)");
    ASSERT_NE(disc, std::string::npos);
    EXPECT_NE(arm, std::string::npos)
        << "INV-3: the reply write must arm the drain guard";
    EXPECT_LT(disc, arm)
        << "INV-3: the guard is armed after disconnectFromServer";
}
