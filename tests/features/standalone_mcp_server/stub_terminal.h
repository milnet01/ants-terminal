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
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>
#include <utility>

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
                    if (req.value(QStringLiteral("method")).toString()
                            == QLatin1String(kTrustPromptMethod)) {
                        answerTrustPrompt(s, req);
                        return;
                    }
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

    // ANTS-5464 — `ants/verifyTrustPrompt`. The handler gets the request's
    // params and returns the reply's `result`, or nullopt to hold the
    // connection open unanswered until releaseHeld(). Without a handler the
    // stub answers -32601, as a terminal without one does.
    static constexpr const char *kTrustPromptMethod = "ants/verifyTrustPrompt";
    using TrustPromptFn =
        std::function<std::optional<QJsonObject>(const QJsonObject &params)>;
    void setTrustPromptHandler(TrustPromptFn fn) { m_trustPrompt = std::move(fn); }
    int trustPromptCount() const {
        int n = 0;
        for (const QJsonObject &r : m_requests)
            if (r.value(QStringLiteral("method")).toString()
                    == QLatin1String(kTrustPromptMethod)) ++n;
        return n;
    }
    void releaseHeld(const QJsonObject &result) {
        for (const auto &[sock, id] : std::as_const(m_held))
            if (sock) replyTo(sock, QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                                                {"result", result}});
        m_held.clear();
    }

private:
    static void replyTo(QLocalSocket *s, const QJsonObject &reply) {
        s->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
        s->flush();
        s->disconnectFromServer();
    }
    void answerTrustPrompt(QLocalSocket *s, const QJsonObject &req) {
        const QJsonValue id = req.value(QStringLiteral("id"));
        if (!m_trustPrompt) {
            replyTo(s, QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                {"error", QJsonObject{{"code", -32601},
                                      {"message", "Method not found"}}}});
            return;
        }
        const std::optional<QJsonObject> result =
            m_trustPrompt(req.value(QStringLiteral("params")).toObject());
        if (!result) {
            m_held.append({QPointer<QLocalSocket>(s), id});
            return;
        }
        replyTo(s, QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", *result}});
    }

    TrustPromptFn m_trustPrompt;
    QList<std::pair<QPointer<QLocalSocket>, QJsonValue>> m_held;
    QString m_path;
    QLocalServer m_server;
    bool m_listening = false;
    QHash<QLocalSocket *, QByteArray> m_pending;
    QList<QJsonObject> m_requests;
};

}  // namespace ants_test
