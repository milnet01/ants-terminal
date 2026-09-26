// ANTS-4932 — drive a real ants-mcpd over stdio from a test.
//
// Shared by tests/features/standalone_mcp_server/ and the ants-mcpd cases in
// tests/features/mcp_tabspecific_contract/. The child inherits the bundle's
// XDG_DATA_HOME / XDG_CONFIG_HOME sandbox (tests/bundle_main_gui.cpp), so it
// opens the same store as the test process and never the user's. Its terminal
// socket is always named: a test that does not supply one gets a path with no
// listener, so nothing is ever forwarded to a running terminal.

#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#ifndef ANTS_MCPD_BIN
#error "ANTS_MCPD_BIN compile definition required"
#endif

namespace ants_test {

class McpdSession {
public:
    // `cwd` becomes the server's process cwd (§ 2.4's fallback root).
    // `terminalSocket` is exported as ANTS_MCP_SOCKET.
    McpdSession(const QString &cwd, const QString &terminalSocket) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("ANTS_MCP_SOCKET"), terminalSocket);
        m_proc.setProcessEnvironment(env);
        m_proc.setWorkingDirectory(cwd);
        m_proc.setProcessChannelMode(QProcess::SeparateChannels);
        m_proc.start(QStringLiteral(ANTS_MCPD_BIN), QStringList{});
        m_started = m_proc.waitForStarted(5000);
        if (m_started)
            send(QStringLiteral("initialize"),
                 QJsonObject{{"protocolVersion", "2024-11-05"},
                             {"capabilities", QJsonObject{}},
                             {"clientInfo", QJsonObject{{"name", "test"},
                                                        {"version", "0"}}}});
    }

    ~McpdSession() {
        m_proc.closeWriteChannel();   // EOF: the server exits
        if (!m_proc.waitForFinished(5000)) m_proc.kill();
    }

    bool started() const { return m_started; }

    // Sends one request and returns its id without waiting.
    int send(const QString &method, const QJsonObject &params = {}) {
        const int id = ++m_nextId;
        QJsonObject req{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.isEmpty()) req["params"] = params;
        m_proc.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
        return id;
    }

    // Bytes written as given, for a request no well-formed client sends.
    void writeRaw(const QByteArray &bytes) { m_proc.write(bytes); }

    int sendCall(const QString &tool, const QJsonObject &args) {
        return send(QStringLiteral("tools/call"),
                    QJsonObject{{"name", tool}, {"arguments", args}});
    }

    // The reply to `id`, or an empty object on timeout. Pumps this process's
    // event loop while it waits, so an in-process stub listener is served.
    QJsonObject await(int id, int timeoutMs = 20000) {
        QElapsedTimer t;
        t.start();
        while (!m_replies.contains(id) && t.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            m_proc.waitForReadyRead(20);
            m_buf += m_proc.readAllStandardOutput();
            qsizetype nl;
            while ((nl = m_buf.indexOf('\n')) >= 0) {
                const QJsonObject o =
                    QJsonDocument::fromJson(m_buf.left(nl)).object();
                m_buf.remove(0, nl + 1);
                if (o.contains(QStringLiteral("id")))
                    m_replies.insert(o.value(QStringLiteral("id")).toInt(), o);
            }
            if (m_proc.state() == QProcess::NotRunning) break;
        }
        // ANTS-5339 — a missing reply says so, and how long was waited. An
        // empty object read as `ok:false` and could not be told from a refusal.
        if (!m_replies.contains(id))
            return QJsonObject{{"test_timeout", true},
                               {"elapsed_ms", double(t.elapsed())},
                               {"server_running",
                                m_proc.state() != QProcess::NotRunning}};
        return m_replies.value(id);
    }

    // A tools/call reply's payload: the JSON object inside the content text,
    // with the `<ants_mcp_data>` wrap stripped. Empty when there is none.
    static QJsonObject payload(const QJsonObject &reply) {
        const QString text = reply.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("content")).toArray()
                                 .at(0).toObject()
                                 .value(QStringLiteral("text")).toString();
        const qsizetype open = text.indexOf(QLatin1Char('{'));
        const qsizetype close = text.lastIndexOf(QLatin1Char('}'));
        if (open < 0 || close < open) return {};
        return QJsonDocument::fromJson(text.mid(open, close - open + 1).toUtf8())
            .object();
    }

    QJsonObject call(const QString &tool, const QJsonObject &args,
                     int timeoutMs = 20000) {
        const QJsonObject reply = await(sendCall(tool, args), timeoutMs);
        if (reply.contains(QStringLiteral("test_timeout")))
            return reply;   // ANTS-5339 — surfaced, not flattened to {}
        return payload(reply);
    }

    // ANTS-5320 — closes stdin, waits for the server to exit, and collects
    // every reply it wrote on the way out. False when it did not exit. Pumps
    // this process's event loop while it waits, so an in-process stub
    // terminal can still answer a forwarded request.
    bool closeInputAndDrain(int timeoutMs = 30000) {
        m_proc.closeWriteChannel();
        QElapsedTimer t;
        t.start();
        bool exited = false;
        while (!exited && t.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            exited = m_proc.waitForFinished(20);
        }
        m_buf += m_proc.readAllStandardOutput();
        qsizetype nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(m_buf.left(nl)).object();
            m_buf.remove(0, nl + 1);
            if (o.contains(QStringLiteral("id")))
                m_replies.insert(o.value(QStringLiteral("id")).toInt(), o);
        }
        return exited;
    }

    bool hasReply(int id) const { return m_replies.contains(id); }

    QByteArray stderrText() { return m_proc.readAllStandardError(); }

private:
    QProcess m_proc;
    bool m_started = false;
    int m_nextId = 0;
    QByteArray m_buf;
    QHash<int, QJsonObject> m_replies;
};

}  // namespace ants_test
