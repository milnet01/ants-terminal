// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llmclient.h"

#include "secretredact.h"

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
    const bool isLocal = (host == QStringLiteral("localhost")
                          || host == QStringLiteral("127.0.0.1")
                          || host == QStringLiteral("::1"));
    return !isLocal;
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

bool LlmClient::accumulateCapped(QString &acc, bool &truncated,
                                 const QString &delta) {
    if (delta.isEmpty()) return false;
    if (acc.size() >= kMaxBytes) {
        if (!truncated) {
            acc += QStringLiteral("\n[response truncated]");
            truncated = true;
        }
        return false;
    }
    acc += delta;
    return true;
}

void LlmClient::send(const LlmRequest &req) {
    abort();
    m_sseLineBuffer.clear();
    m_text.clear();
    m_truncated = false;
    m_redactedCount = 0;

    QString scheme;
    if (!isEndpointAllowed(req.endpoint, &scheme)) {
        emitDeferredError(
            QStringLiteral("AI endpoint rejected — only http/https are "
                           "permitted (got '%1').").arg(scheme));
        return;
    }

    const QByteArray bodyBytes = buildRequestBody(req, &m_redactedCount);

    QNetworkRequest httpReq{ QUrl(req.endpoint) };
    httpReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    httpReq.setTransferTimeout(req.timeoutMs);
    if (!req.apiKey.isEmpty())
        httpReq.setRawHeader("Authorization", ("Bearer " + req.apiKey).toUtf8());

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

    // Cap the line buffer to guard against a misbehaving server.
    if (m_sseLineBuffer.size() > kMaxBytes) {
        m_sseLineBuffer.clear();
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
        if (accumulateCapped(m_text, m_truncated, delta))
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
