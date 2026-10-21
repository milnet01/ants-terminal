// ANTS-4932 § 2.5 — ants-mcpd's forwarder for the terminal-scoped verbs.
//
// Opens a fresh connection to the terminal's MCP socket per request, relays
// the request line verbatim, and relays the reply line verbatim: it does not
// parse, re-wrap, re-cap or re-sanitise, because the terminal's reply is
// already a complete envelope. On a failure it hands back a `no_terminal`
// envelope for the pipeline to wrap like any other refusal, or
// `terminal_timeout` when the terminal took the request and missed the
// deadline (ANTS-4845).

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

namespace mcpd {

class Forwarder : public QObject {
public:
    using ReplyFn = std::function<void(const QByteArray &replyLine)>;
    using FailFn  = std::function<void(const QString &envelopeJson)>;

    explicit Forwarder(QObject *parent = nullptr) : QObject(parent) {}

    // Exactly one of the two callbacks runs, once, on this object's thread.
    void forward(const QByteArray &requestLine, ReplyFn onReply, FailFn onFail);

    // The `no_terminal` envelope (§ 2.5), with `detail` naming why.
    static QString noTerminalEnvelope(const QString &detail);

    // The `terminal_timeout` envelope: the request was sent and no reply came
    // before the deadline. The terminal may still carry the call out.
    static QString terminalTimeoutEnvelope(const QString &detail);

    // The connect-and-reply deadline. 60 s; a test shortens it.
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

private:
    int m_timeoutMs = 60 * 1000;
};

}  // namespace mcpd
