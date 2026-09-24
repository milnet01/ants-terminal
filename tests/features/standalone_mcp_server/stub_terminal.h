// ANTS-4932 — a stand-in for the terminal's MCP socket.
//
// Records every request line ants-mcpd forwards and answers each with a
// canned reply for the same id, whose payload carries {"stub":true} so a test
// can tell a relayed reply from a refusal. One request per connection, as the
// terminal serves them.

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>

namespace ants_test {

class StubTerminal {
public:
    explicit StubTerminal(const QString &path) : m_path(path) {
        QLocalServer::removeServer(path);
        m_listening = m_server.listen(path);
        QObject::connect(&m_server, &QLocalServer::newConnection, &m_server, [this] {
            while (QLocalSocket *s = m_server.nextPendingConnection()) {
                QObject::connect(s, &QLocalSocket::readyRead, s, [this, s] {
                    m_pending[s] += s->readAll();
                    const qsizetype nl = m_pending[s].indexOf('\n');
                    if (nl < 0) return;
                    const QJsonObject req =
                        QJsonDocument::fromJson(m_pending[s].left(nl)).object();
                    m_pending.remove(s);
                    m_requests << req;
                    const QJsonObject reply{
                        {"jsonrpc", "2.0"},
                        {"id", req.value(QStringLiteral("id"))},
                        {"result", QJsonObject{{"content", QJsonArray{QJsonObject{
                            {"type", "text"}, {"text", R"({"ok":true,"stub":true})"}}}}}}};
                    s->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                    s->flush();
                    s->disconnectFromServer();
                });
                QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }

    bool listening() const { return m_listening; }
    QString path() const { return m_path; }
    const QList<QJsonObject> &requests() const { return m_requests; }

private:
    QString m_path;
    QLocalServer m_server;
    bool m_listening = false;
    QHash<QLocalSocket *, QByteArray> m_pending;
    QList<QJsonObject> m_requests;
};

}  // namespace ants_test
