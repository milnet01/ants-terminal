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

#include <limits>

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
    // ANTS-5002 — go through abort(), which nulls m_reply BEFORE aborting:
    // QNetworkReply::abort() emits finished() synchronously, and with the
    // pointer still set onFinished() emitted from this dying object and then
    // left the destructor dereferencing null.
    abort();
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

QString LlmClient::endpointEgressError(const QString &endpoint,
                                       const QString &apiKey) {
    QString scheme;
    if (!isEndpointAllowed(endpoint, &scheme))
        return QStringLiteral(
                   "only http/https are permitted (got '%1').").arg(scheme);
    // ANTS-2109 H1 — refuse an endpoint that embeds URL userinfo
    // (https://user:pass@host). Qt would derive an `Authorization: Basic`
    // header from it and POST those credentials verbatim — unscrubbed, in
    // addition to the Bearer key, to a value the user may have imported rather
    // than typed. The host-keyed scheme/SSRF gates all run on QUrl::host()
    // (which strips userinfo), so this channel is otherwise invisible. Refuse
    // rather than silently strip so the surprising config surfaces.
    // Scheme-agnostic — https leaks the creds too.
    if (!QUrl(endpoint).userInfo().isEmpty())
        return QStringLiteral(
            "the URL embeds credentials (user:pass@host) that would be sent "
            "unscrubbed. Remove the userinfo and use the API-key field.");
    // ANTS-1746 — refuse SSRF-shaped endpoints (cloud-metadata
    // 169.254.169.254, RFC-1918, link-local, ULA). Loopback + hostnames pass.
    if (isEndpointHostBlocked(endpoint))
        return QStringLiteral(
            "host is a private, link-local, or cloud-metadata address (SSRF "
            "guard). Use a public endpoint or a localhost server.");
    // ANTS-1826/2108 — never ship the Bearer key in cleartext to a remote
    // host. Gate on a non-empty key; loopback/localhost stay exempt via
    // isPlaintextRemote so a local dev LLM server still works keyed.
    if (!apiKey.isEmpty() && isPlaintextRemote(endpoint))
        return QStringLiteral(
            "refusing to send the API key over cleartext http to a remote "
            "host. Use https (localhost is exempt).");
    return QString();
}

QString LlmClient::plaintextPromptWarning(const QString &endpoint,
                                         const QString &apiKey) {
    // ANTS-5010 — stub for the test-first run; § 2.1 of the spec lands next.
    Q_UNUSED(endpoint);
    Q_UNUSED(apiKey);
    return QString();
}

void LlmClient::send(const LlmRequest &req) {
    abort();
    ++m_sendGeneration;   // ANTS-2019 — invalidate any pending deferred error
    m_sseLineBuffer.clear();
    m_text.clear();
    m_textBytes = 0;  // ANTS-1846 — reset the byte counter alongside m_text
    m_truncated = false;
    m_redactedCount = 0;
    m_rawBody.clear();
    m_sawSse = false;
    m_stoppedAtCap = false;

    // ANTS-2121 — all four egress gates (scheme / URL-userinfo / SSRF /
    // cleartext-remote Bearer) live in one shared validator so the AuditDialog
    // AI-triage POSTs enforce the identical policy. The verbatim rejection
    // messages are preserved (prefixed here); the redirect refusal below is a
    // request attribute, not a validation, so it stays in send().
    const QString egressErr = endpointEgressError(req.endpoint, req.apiKey);
    if (!egressErr.isEmpty()) {
        emitDeferredError(QStringLiteral("AI endpoint rejected — ") + egressErr);
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
    // ANTS-2019 — gate on the send generation captured now: if send() runs
    // again before the event loop fires this, the callback no-ops instead of
    // emitting a stale error for a request that has since been superseded.
    const quint64 gen = m_sendGeneration;
    QTimer::singleShot(0, this, [this, r, gen]() {
        if (gen != m_sendGeneration) return;
        emit finished(r);
    });
}

void LlmClient::drain() {
    if (!m_reply) return;
    const QByteArray bytes = m_reply->readAll();
    // ANTS-5007 — keep the raw body until a data line proves this is a stream.
    if (!m_sawSse)
        m_rawBody += bytes.left(kMaxBytes - m_rawBody.size());
    m_sseLineBuffer += bytes;

    // Cap the line buffer to guard against a misbehaving server. A single
    // SSE frame larger than the cap (no newline in kMaxBytes of bytes) is
    // pathological — drop the buffered partial frame, but flag the answer
    // as truncated so onFinished() doesn't report it as complete. Without
    // the flag the caller silently resumes parsing on a corrupted offset
    // and treats a mangled answer as whole (ANTS-1754).
    // Cap per-tick iterations + re-arm via singleShot(0) so a flood of
    // tiny SSE lines can't hold the event loop (UI freeze).
    constexpr int kMaxLinesPerTick = 256;
    if (m_sseLineBuffer.size() > kMaxBytes) {
        m_sseLineBuffer.clear();
        if (!m_truncated) {
            m_truncated = true;
            m_text += QStringLiteral("\n[response truncated]");
        }
    } else if (consumeLines(kMaxLinesPerTick) && !m_truncated) {
        QTimer::singleShot(0, this, &LlmClient::drain);
    }

    // ANTS-5008 — the cap ends the download, not just the appending. abort()
    // delivers finished() to onFinished(), which reports the capped answer.
    // Last statement: a finished() slot may delete this client.
    if (m_truncated) {
        m_stoppedAtCap = true;
        m_reply->abort();
    }
}

bool LlmClient::consumeLines(int maxLines) {
    // ANTS-5005 — walk by offset and trim once: a per-line mid() copied the
    // whole remaining buffer for every line.
    qsizetype consumed = 0;
    for (int processed = 0; processed < maxLines; ++processed) {
        const qsizetype nlPos = m_sseLineBuffer.indexOf('\n', consumed);
        if (nlPos < 0) break;
        const QString line = QString::fromUtf8(
            m_sseLineBuffer.constData() + consumed, nlPos - consumed);
        consumed = nlPos + 1;

        // ANTS-5007 — a data line proves the reply is a stream.
        if (!m_sawSse && line.trimmed().startsWith(QStringLiteral("data:"))) {
            m_sawSse = true;
            m_rawBody.clear();
        }

        const QString delta = sseContentDelta(line);
        if (accumulateCapped(m_text, m_textBytes, m_truncated, delta))
            emit chunk(delta);
    }
    m_sseLineBuffer.remove(0, consumed);
    return m_sseLineBuffer.contains('\n');
}

void LlmClient::onFinished() {
    if (!m_reply) return;

    // ANTS-5015 — drain() parses kMaxLinesPerTick lines and re-arms itself,
    // and the reply can finish before the re-armed call runs; that call then
    // returns on the null m_reply. Parse every complete line still buffered.
    consumeLines(std::numeric_limits<int>::max());

    LlmResult result;
    result.httpStatus =
        m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // ANTS-5008 — an abort drain() made at the cap is not a failed request.
    bool hadError =
        m_reply->error() != QNetworkReply::NoError && !m_stoppedAtCap;

    // ANTS-5007 — no SSE data line arrived, so this is a plain JSON body: a
    // provider that ignores "stream": true, or an error body. Read it on
    // success and error alike, from the copy drain() kept (capped at
    // kMaxBytes, indie-review 2026-06-04).
    if (!m_sawSse && !m_rawBody.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(m_rawBody);
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
                // Scrub the server-supplied error like every other error
                // surface here — a 4xx body can echo a submitted key back
                // (OWASP LLM06). indie-review 2026-06-04.
                result.error = scrubErrorString(
                    obj.value("error").toObject().value("message").toString());
                hadError = true;  // an error body with no answer fails, even on 2xx
            }
        }
    }
    if (hadError && m_text.isEmpty() && result.error.isEmpty())
        result.error = scrubErrorString(m_reply->errorString());
    else if (hadError && !m_text.isEmpty())
        m_text += QStringLiteral("\n[response may be incomplete]");

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
