// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llmclient.h"

#include "secretredact.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace {
// Qt's QNetworkReply::errorString() can embed the full endpoint URL —
// including any `user:pass@` userinfo from a credentialed ai_endpoint —
// which then lands in the chat history / screenshots. Strip userinfo and
// run the generic secret scrub before surfacing. indie-review-2026-05-21.
QString scrubErrorString(const QString &s) {
    static const QRegularExpression rxUserInfo(
        QStringLiteral("://[^/?#@\\s]*@"));
    QString out = s;
    out.replace(rxUserInfo, QStringLiteral("://"));
    return SecretRedact::scrub(out).text;
}
}  // namespace

LlmClient::LlmClient(QObject *parent) : QObject(parent) {}

LlmClient::~LlmClient() {
    // Abort and drop any in-flight reply before member teardown so a late
    // readyRead/finished can't fire on a partially-destructed client.
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

bool LlmClient::busy() const { return m_reply != nullptr; }

void LlmClient::abort() {
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
}

bool LlmClient::isEndpointAllowed(const QString &endpoint, QString *schemeOut) {
    if (endpoint.isEmpty()) return false;
    const QUrl u(endpoint);
    const QString scheme = u.scheme().toLower();
    if (schemeOut) *schemeOut = scheme;
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
}

bool LlmClient::isPlaintextRemote(const QString &endpoint) {
    const QUrl u(endpoint);
    if (u.scheme().toLower() != QStringLiteral("http")) return false;
    const QString host = u.host();
    // ANTS-1846 — unify with isEndpointHostBlocked's loopback handling.
    // QHostAddress::isLoopback covers all of 127.0.0.0/8 + ::1, not just the
    // three literals the old set listed — 127.0.0.2 was wrongly treated as a
    // remote host, producing a spurious cleartext warning for a local dev LLM.
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0)
        return false;
    const QHostAddress addr(host);
    if (!addr.isNull() && addr.isLoopback()) return false;
    return true;
}

// ANTS-1798 — private / link-local / metadata / special IPv4 ranges.
// Factored out so the IPv6 embedded-IPv4 path (NAT64, IPv4-compatible)
// applies the identical test on the trailing 32 bits.
static bool isBlockedV4(quint32 v4) {
    const quint32 a = (v4 >> 24) & 0xff;
    const quint32 b = (v4 >> 16) & 0xff;
    if (a == 10) return true;                          // 10.0.0.0/8
    if (a == 172 && b >= 16 && b <= 31) return true;   // 172.16.0.0/12
    if (a == 192 && b == 168) return true;             // 192.168.0.0/16
    if (a == 169 && b == 254) return true;             // 169.254.0.0/16 (metadata)
    if (a == 100 && b >= 64 && b <= 127) return true;  // 100.64.0.0/10 (CGNAT)
    if (a == 0) return true;                           // 0.0.0.0/8
    return false;
}

bool LlmClient::isEndpointHostBlocked(const QString &endpoint) {
    const QString host = QUrl(endpoint).host();
    if (host.isEmpty()) return false;  // scheme gate handles empties
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0)
        return false;                  // local dev LLM server

    const QHostAddress addr(host);
    if (addr.isNull()) return false;   // DNS hostname — not resolved here
    if (addr.isLoopback()) return false;  // 127.0.0.0/8, ::1

    bool isV4 = false;
    const quint32 v4 = addr.toIPv4Address(&isV4);  // also unwraps ::ffff:a.b.c.d
    if (isV4) return isBlockedV4(v4);

    // IPv6 literal.
    if (addr.isLinkLocal()) return true;            // fe80::/10
    if (addr.isUniqueLocalUnicast()) return true;   // fc00::/7
    // ANTS-1798 — embedded-IPv4 forms that toIPv4Address does NOT unwrap:
    // NAT64 well-known prefix 64:ff9b::/96 and the deprecated IPv4-compatible
    // ::/96. A NAT64 gateway would route e.g. [64:ff9b::169.254.169.254] to
    // the metadata IP, so extract the trailing 32 bits and apply the v4 test.
    const Q_IPV6ADDR a6 = addr.toIPv6Address();
    auto bytesZero = [&](int from, int to) {
        for (int i = from; i <= to; ++i)
            if (a6[i] != 0) return false;
        return true;
    };
    const bool nat64 = (a6[0] == 0x00 && a6[1] == 0x64 && a6[2] == 0xff &&
                        a6[3] == 0x9b && bytesZero(4, 11));
    const bool v4compat = bytesZero(0, 11);  // ::/96 (loopback handled above)
    if (nat64 || v4compat) {
        const quint32 embedded =
            (quint32(a6[12]) << 24) | (quint32(a6[13]) << 16) |
            (quint32(a6[14]) << 8) | quint32(a6[15]);
        return isBlockedV4(embedded);
    }
    return false;
}

QString LlmClient::sseContentDelta(const QString &dataLine) {
    const QString line = dataLine.trimmed();
    if (!line.startsWith(QStringLiteral("data:"))) return QString();
    // Handle both "data: " (standard) and "data:" (some providers).
    const QString json =
        line.mid(line.startsWith(QStringLiteral("data: ")) ? 6 : 5).trimmed();
    if (json == QStringLiteral("[DONE]")) return QString();
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return QString();
    const QJsonArray choices = doc.object().value("choices").toArray();
    if (choices.isEmpty()) return QString();
    return choices[0].toObject().value("delta").toObject()
        .value("content").toString();
}

QByteArray LlmClient::buildRequestBody(const LlmRequest &req,
                                       int *redactedCount) {
    QString systemContent = req.systemPrompt;
    QString userContent = req.userPrompt;
    int redacted = 0;
    if (req.scrubSecrets) {
        // OWASP LLM06 — scrub well-known secret shapes out of both prompts
        // before either leaves the process.
        const auto s = SecretRedact::scrub(systemContent);
        const auto u = SecretRedact::scrub(userContent);
        systemContent = s.text;
        userContent = u.text;
        redacted = s.redactedCount + u.redactedCount;
    }
    if (redactedCount) *redactedCount = redacted;

    QJsonObject systemMsg{ {"role", "system"}, {"content", systemContent} };
    QJsonObject userMsg{ {"role", "user"}, {"content", userContent} };
    QJsonArray messages{ systemMsg, userMsg };
    QJsonObject body{
        {"model", req.model},
        {"messages", messages},
        {"stream", true},
        {"max_tokens", req.maxTokens},
    };
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool LlmClient::accumulateCapped(QString &acc, qint64 &accBytes,
                                 bool &truncated, const QString &delta) {
    if (delta.isEmpty()) return false;
    // ANTS-1846 — gate on the exact UTF-8 byte total, not QString::size()
    // (UTF-16 units), so the cap means kMaxBytes of decoded content.
    if (accBytes >= kMaxBytes) {
        if (!truncated) {
            acc += QStringLiteral("\n[response truncated]");
            truncated = true;
        }
        return false;
    }
    acc += delta;
    accBytes += delta.toUtf8().size();
    return true;
}

void LlmClient::send(const LlmRequest &req) {
    abort();
    m_sseLineBuffer.clear();
    m_text.clear();
    m_textBytes = 0;  // ANTS-1846 — reset the byte counter alongside m_text
    m_truncated = false;
    m_redactedCount = 0;

    QString scheme;
    if (!isEndpointAllowed(req.endpoint, &scheme)) {
        emitDeferredError(
            QStringLiteral("AI endpoint rejected — only http/https are "
                           "permitted (got '%1').").arg(scheme));
        return;
    }
    if (isEndpointHostBlocked(req.endpoint)) {
        // ANTS-1746 — refuse SSRF-shaped endpoints (cloud-metadata
        // 169.254.169.254, RFC-1918, link-local, ULA). Loopback +
        // hostnames still pass.
        emitDeferredError(
            QStringLiteral("AI endpoint rejected — host is a private, "
                           "link-local, or cloud-metadata address (SSRF "
                           "guard). Use a public endpoint or a localhost "
                           "server."));
        return;
    }

    const QByteArray bodyBytes = buildRequestBody(req, &m_redactedCount);

    QNetworkRequest httpReq{ QUrl(req.endpoint) };
    httpReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    httpReq.setTransferTimeout(req.timeoutMs);
    if (!req.apiKey.isEmpty())
        httpReq.setRawHeader("Authorization", ("Bearer " + req.apiKey).toUtf8());

    // ANTS-1798 — do NOT auto-follow redirects. Qt's default
    // NoLessSafeRedirectPolicy would follow a 3xx Location into a host the
    // SSRF guard (isEndpointHostBlocked) never re-validates — a hostile
    // endpoint returning `307 Location: http://169.254.169.254/...` would
    // defeat the allowlist and re-POST the body to cloud metadata. Refuse
    // redirects outright; AI chat endpoints do not legitimately redirect.
    httpReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    m_reply = m_net.post(httpReq, bodyBytes);
    connect(m_reply, &QNetworkReply::readyRead, this, &LlmClient::drain);
    connect(m_reply, &QNetworkReply::finished, this, &LlmClient::onFinished);
}

void LlmClient::emitDeferredError(const QString &error) {
    // Defer so send() always completes before any slot runs (a slot might
    // delete this client).
    LlmResult r;
    r.ok = false;
    r.error = error;
    QTimer::singleShot(0, this, [this, r]() { emit finished(r); });
}

void LlmClient::drain() {
    if (!m_reply) return;
    m_sseLineBuffer += m_reply->readAll();

    // Cap the line buffer to guard against a misbehaving server. A single
    // SSE frame larger than the cap (no newline in kMaxBytes of bytes) is
    // pathological — drop the buffered partial frame, but flag the answer
    // as truncated so onFinished() doesn't report it as complete. Without
    // the flag the caller silently resumes parsing on a corrupted offset
    // and treats a mangled answer as whole (ANTS-1754).
    if (m_sseLineBuffer.size() > kMaxBytes) {
        m_sseLineBuffer.clear();
        if (!m_truncated) {
            m_truncated = true;
            m_text += QStringLiteral("\n[response truncated]");
        }
        return;
    }

    // Cap per-tick iterations + re-arm via singleShot(0) so a flood of
    // tiny SSE lines can't hold the event loop (UI freeze).
    constexpr int kMaxLinesPerTick = 256;
    int processed = 0;
    while (processed < kMaxLinesPerTick) {
        const int nlPos = m_sseLineBuffer.indexOf('\n');
        if (nlPos < 0) break;
        ++processed;
        const QString line =
            QString::fromUtf8(m_sseLineBuffer.left(nlPos));
        m_sseLineBuffer = m_sseLineBuffer.mid(nlPos + 1);

        const QString delta = sseContentDelta(line);
        if (accumulateCapped(m_text, m_textBytes, m_truncated, delta))
            emit chunk(delta);
    }

    if (processed >= kMaxLinesPerTick && m_sseLineBuffer.indexOf('\n') >= 0)
        QTimer::singleShot(0, this, &LlmClient::drain);
}

void LlmClient::onFinished() {
    if (!m_reply) return;

    LlmResult result;
    result.httpStatus =
        m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bool hadError = m_reply->error() != QNetworkReply::NoError;

    if (hadError) {
        // Some APIs don't stream — try a non-streaming JSON response.
        const QByteArray data = m_reply->readAll();
        if (!data.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                const QJsonArray choices = obj.value("choices").toArray();
                if (!choices.isEmpty()) {
                    const QString content = choices[0].toObject()
                        .value("message").toObject().value("content").toString();
                    if (!content.isEmpty()) {
                        m_text = content;
                        hadError = false;  // valid response despite HTTP error
                    }
                } else if (obj.contains("error")) {
                    result.error =
                        obj.value("error").toObject().value("message").toString();
                }
            }
        }
        if (hadError && m_text.isEmpty() && result.error.isEmpty())
            result.error = scrubErrorString(m_reply->errorString());
        else if (hadError && !m_text.isEmpty())
            m_text += QStringLiteral("\n[response may be incomplete]");
    }

    result.ok = !hadError;
    result.text = m_text;
    result.truncated = m_truncated;
    result.redactedCount = m_redactedCount;
    if (result.error.isEmpty() && hadError)
        result.error = scrubErrorString(m_reply->errorString());

    m_reply->deleteLater();
    m_reply = nullptr;
    emit finished(result);
}
