// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// LlmClient — reusable OpenAI-compatible streaming chat client, extracted
// from AiDialog (ANTS-1727). Qt6::Core + Qt6::Network only; no Qt Widgets
// include (INV-16 source-grep guards this). The request build, SSE drain,
// 10 MiB caps, scheme allowlist, transfer timeout, and OWASP LLM06 secret
// scrub all live here so non-widget callers (LlmDispatcher, the review
// dialogs) can reach them.

#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QNetworkReply;

struct LlmRequest {
    QString endpoint;        // http/https only (validated)
    QString apiKey;          // Bearer; may be empty (local Ollama/LM Studio)
    QString model;
    QString systemPrompt;
    QString userPrompt;
    int     maxTokens = 1024;
    int     timeoutMs = 30000;
    bool    scrubSecrets = true;   // OWASP LLM06 — scrub before send
};

struct LlmResult {
    bool    ok = false;
    QString text;            // accumulated assistant content
    QString error;           // human-readable; empty on ok
    int     redactedCount = 0;
    bool    truncated = false;   // hit the 10 MiB accumulation cap
    int     httpStatus = 0;
};

class LlmClient : public QObject {
    Q_OBJECT
public:
    explicit LlmClient(QObject *parent = nullptr);
    ~LlmClient() override;            // aborts in-flight reply

    void send(const LlmRequest &req); // single in-flight; aborts prior
    void abort();
    bool busy() const;

    // ---- Widget-free helpers, testable without a network round-trip ----

    // Accumulated-content + SSE-line-buffer cap (mirrors aidialog.cpp).
    static constexpr qint64 kMaxBytes = 10 * 1024 * 1024;

    // True iff `endpoint` parses with an http/https scheme. On true,
    // *schemeOut (when non-null) receives the lowercased scheme.
    static bool isEndpointAllowed(const QString &endpoint, QString *schemeOut = nullptr);

    // True iff endpoint is plaintext http to a non-local host (caller
    // surfaces the "Bearer travels in cleartext" warning).
    static bool isPlaintextRemote(const QString &endpoint);

    // Parse one SSE "data:{…}" line → choices[0].delta.content. Empty for
    // "[DONE]", non-data lines, and non-content events.
    static QString sseContentDelta(const QString &dataLine);

    // Build the scrubbed OpenAI-compatible JSON request body. When
    // req.scrubSecrets, systemPrompt + userPrompt are run through
    // SecretRedact; *redactedCount (when non-null) receives the total.
    static QByteArray buildRequestBody(const LlmRequest &req,
                                       int *redactedCount = nullptr);

    // Append `delta` to `acc` unless the kMaxBytes cap is reached; on the
    // first overflow sets `truncated` and appends a one-time marker.
    // Returns true if `delta` was appended.
    static bool accumulateCapped(QString &acc, bool &truncated,
                                 const QString &delta);

signals:
    void chunk(const QString &delta);        // streamed content
    void finished(const LlmResult &result);

private:
    void drain();
    void onFinished();
    void emitDeferredError(const QString &error);

    QNetworkAccessManager m_net;
    QNetworkReply *m_reply = nullptr;
    QByteArray     m_sseLineBuffer;   // buffers incomplete SSE lines
    QString        m_text;            // accumulated assistant content
    bool           m_truncated = false;
    int            m_redactedCount = 0;
};
