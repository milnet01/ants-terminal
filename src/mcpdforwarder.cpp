#include "mcpdforwarder.h"

#include "mcpdsocket.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include <memory>
#include <unistd.h>

namespace mcpd {

namespace {
// Same ceilings as tools/mcp-bridge.py: the terminal's reply cap, and the
// bridge's connect and read timeouts.
constexpr qsizetype kMaxReplyBytes = 10 * 1024 * 1024;
constexpr int kTimeoutMs = 60 * 1000;
}  // namespace

QString Forwarder::noTerminalEnvelope(const QString &detail) {
    QJsonObject env;
    env[QStringLiteral("ok")]    = false;
    env[QStringLiteral("code")]  = QStringLiteral("no_terminal");
    env[QStringLiteral("error")] = detail.isEmpty()
        ? QStringLiteral("no Ants Terminal is running")
        : QStringLiteral("no Ants Terminal is running (%1)").arg(detail);
    env[QStringLiteral("hint")]  = QStringLiteral(
        "start Ants Terminal, or pass caller_cwd to a project-scoped verb");
    return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
}

void Forwarder::forward(const QByteArray &requestLine, ReplyFn onReply,
                        FailFn onFail) {
    const uid_t me = ::getuid();
    QString why;
    const QString path = pickTerminalSocket(me, &why);
    if (path.isEmpty()) {
        onFail(noTerminalEnvelope(why));
        return;
    }

    struct State {
        bool done = false;
        QByteArray buf;
        ReplyFn onReply;
        FailFn onFail;
    };
    auto st = std::make_shared<State>();
    st->onReply = std::move(onReply);
    st->onFail  = std::move(onFail);

    auto *sock  = new QLocalSocket(this);
    auto *timer = new QTimer(sock);
    timer->setSingleShot(true);

    // Every exit goes through here, so exactly one callback runs.
    auto finish = [st, sock](bool ok, const QString &detail) {
        if (st->done) return;
        st->done = true;
        sock->disconnect();
        sock->abort();
        sock->deleteLater();
        if (ok) st->onReply(st->buf);
        else    st->onFail(noTerminalEnvelope(detail));
    };

    connect(timer, &QTimer::timeout, sock, [finish, path] {
        finish(false, QStringLiteral("%1 did not answer within %2 s")
                          .arg(path).arg(kTimeoutMs / 1000));
    });
    connect(sock, &QLocalSocket::connected, sock,
            [sock, me, finish, path, requestLine] {
        // INV-11 — the second check, on the connected peer.
        if (!peerUidIs(static_cast<int>(sock->socketDescriptor()), me)) {
            finish(false, QStringLiteral("the peer on %1 is not uid %2 (uid check)")
                              .arg(path).arg(me));
            return;
        }
        QByteArray out = requestLine;
        if (!out.endsWith('\n')) out.append('\n');
        sock->write(out);
        sock->flush();
    });
    connect(sock, &QLocalSocket::readyRead, sock, [sock, st, finish] {
        st->buf += sock->readAll();
        if (st->buf.size() > kMaxReplyBytes) {
            finish(false, QStringLiteral("reply exceeded 10 MiB"));
            return;
        }
        // ANTS-1769 — '\n' ends a reply; compact JSON carries no raw newline.
        const qsizetype nl = st->buf.indexOf('\n');
        if (nl >= 0) {
            st->buf.truncate(nl + 1);
            finish(true, {});
        }
    });
    connect(sock, &QLocalSocket::disconnected, sock, [st, finish, path] {
        if (st->buf.isEmpty()) {
            finish(false, QStringLiteral("%1 closed without replying").arg(path));
            return;
        }
        // A reply cut off before its terminator is not a reply.
        finish(false, QStringLiteral("%1 closed mid-reply").arg(path));
    });
    connect(sock, &QLocalSocket::errorOccurred, sock,
            [sock, st, finish, path](QLocalSocket::LocalSocketError) {
        // PeerClosedError after a complete reply is handled by readyRead.
        if (st->done) return;
        finish(false, QStringLiteral("%1: %2").arg(path, sock->errorString()));
    });

    timer->start(kTimeoutMs);
    sock->connectToServer(path);
}

}  // namespace mcpd
