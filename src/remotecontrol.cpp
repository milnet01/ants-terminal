#include "remotecontrol.h"
#include "mainwindow.h"
#include "terminalwidget.h"
#include "debuglog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTabWidget>

RemoteControl::RemoteControl(MainWindow *main, QObject *parent)
    : QObject(parent), m_main(main) {}

RemoteControl::~RemoteControl() {
    if (m_server) {
        m_server->close();
    }
}

QString RemoteControl::defaultSocketPath() {
    // Override wins unconditionally — lets the user script
    // multi-instance setups without touching the source.
    const QByteArray override = qgetenv("ANTS_REMOTE_SOCKET");
    if (!override.isEmpty()) return QString::fromLocal8Bit(override);

    const QString xdg = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (!xdg.isEmpty()) {
        return xdg + "/ants-terminal.sock";
    }
    return QStringLiteral("/tmp/ants-terminal-%1.sock")
        .arg(::getuid());
}

bool RemoteControl::start() {
    if (m_server) return true;

    const QString path = defaultSocketPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_server = new QLocalServer(this);
    // Restrict access to the owning user — matches the hook/MCP
    // sockets' posture. Must be set before listen() on Unix; Qt
    // enforces this on the socket itself.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    // If a stale socket file exists (previous crash didn't clean up),
    // remove it. `removeServer` is a no-op if no socket exists and
    // succeeds when the path exists but is not actively bound.
    // If another live instance holds the lock, listen() fails and
    // we skip the takeover (see outer `if` below).
    if (!m_server->listen(path)) {
        QLocalServer::removeServer(path);
        if (!m_server->listen(path)) {
            ANTS_LOG(DebugLog::Network,
                "remote-control: listen(%s) failed — another instance "
                "may own the socket; remote-control disabled for this "
                "process", qUtf8Printable(path));
            delete m_server;
            m_server = nullptr;
            return false;
        }
    }
    QFile::setPermissions(path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    connect(m_server, &QLocalServer::newConnection,
            this, &RemoteControl::onNewConnection);
    ANTS_LOG(DebugLog::Network,
        "remote-control: listening on %s", qUtf8Printable(path));
    return true;
}

void RemoteControl::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        // Line-buffer incoming data. Each connection handles exactly
        // one request/response round-trip today — simpler than a
        // persistent-session protocol and good enough for the full
        // Kitty command set (which is also one-shot).
        socket->setProperty("_buf", QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            QByteArray buf = socket->property("_buf").toByteArray();
            buf += socket->readAll();
            // Bound the in-memory buffer for defence-in-depth against
            // a malicious client on the same machine. 1 MB is far
            // more than any realistic Kitty rc_protocol envelope.
            if (buf.size() > 1 * 1024 * 1024) {
                socket->disconnectFromServer();
                return;
            }
            socket->setProperty("_buf", buf);

            int nlIdx = buf.indexOf('\n');
            if (nlIdx < 0) return;  // partial line, wait for more

            const QByteArray line = buf.left(nlIdx);
            QJsonParseError err;
            QJsonDocument req = QJsonDocument::fromJson(line, &err);
            QJsonDocument resp;
            if (err.error != QJsonParseError::NoError || !req.isObject()) {
                QJsonObject e;
                e["ok"] = false;
                e["error"] = QStringLiteral("invalid JSON: %1")
                    .arg(err.errorString());
                resp = QJsonDocument(e);
            } else {
                resp = dispatch(req.object());
            }
            socket->write(resp.toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
            socket->disconnectFromServer();
        });
        connect(socket, &QLocalSocket::disconnected,
                socket, &QLocalSocket::deleteLater);
    }
}

QJsonDocument RemoteControl::dispatch(const QJsonObject &req) {
    const QString cmd = req.value("cmd").toString();
    if (cmd == QLatin1String("ls")) {
        return cmdLs();
    }
    QJsonObject e;
    e["ok"] = false;
    e["error"] = QStringLiteral("unknown command: %1").arg(cmd);
    return QJsonDocument(e);
}

QJsonDocument RemoteControl::cmdLs() {
    QJsonObject out;
    out["ok"] = true;
    out["tabs"] = m_main->tabListForRemote();
    return QJsonDocument(out);
}

// Client — runs in the --remote invocation of the binary. No Qt
// event loop; synchronous connect → write → readLine → exit.
int RemoteControl::runClient(const QString &command,
                             const QJsonObject &args,
                             const QString &socketPath) {
    QJsonObject env = args;
    env["cmd"] = command;
    const QByteArray payload = QJsonDocument(env).toJson(
        QJsonDocument::Compact) + '\n';

    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(2000)) {
        fprintf(stderr,
            "ants-terminal --remote: cannot connect to %s (%s)\n"
            "  Is Ants Terminal running with remote-control enabled?\n"
            "  Override the path via ANTS_REMOTE_SOCKET=...\n",
            qUtf8Printable(socketPath),
            qUtf8Printable(socket.errorString()));
        return 1;
    }
    socket.write(payload);
    if (!socket.waitForBytesWritten(2000)) {
        fprintf(stderr, "ants-terminal --remote: write timeout\n");
        return 1;
    }
    // Read until newline or disconnect.
    QByteArray resp;
    while (socket.waitForReadyRead(2000)) {
        resp += socket.readAll();
        if (resp.contains('\n')) break;
    }
    if (resp.isEmpty()) {
        fprintf(stderr, "ants-terminal --remote: no response\n");
        return 1;
    }
    // Strip trailing newline for tidier stdout.
    while (!resp.isEmpty() && (resp.endsWith('\n') || resp.endsWith('\r'))) {
        resp.chop(1);
    }
    fwrite(resp.constData(), 1, resp.size(), stdout);
    fputc('\n', stdout);

    // Exit-code shaping: parse the "ok" field so callers can
    // `if ants-terminal --remote ls; then ...` without piping
    // through jq.
    QJsonDocument doc = QJsonDocument::fromJson(resp);
    if (doc.isObject() && doc.object().value("ok").toBool()) return 0;
    return 2;
}
