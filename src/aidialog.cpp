#include "aidialog.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QNetworkRequest>
#include <QScrollBar>
#include <QUrl>

AiDialog::~AiDialog() {
    // Abort and drop any in-flight reply before member teardown runs. Qt
    // auto-disconnects signals from a destroyed receiver, but there's a
    // narrow window during member destruction where slots can still fire
    // on a partially-destructed AiDialog. Explicit abort closes the window.
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
}

AiDialog::AiDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("AI Assistant");
    setMinimumSize(500, 400);
    resize(600, 500);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Status label
    m_statusLabel = new QLabel("Configure AI endpoint in config.json", this);
    m_statusLabel->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(m_statusLabel);

    // Chat history
    m_chatHistory = new QTextEdit(this);
    m_chatHistory->setReadOnly(true);
    m_chatHistory->setPlaceholderText("Ask about terminal output, get command suggestions, or debug errors...");
    layout->addWidget(m_chatHistory, 1);

    // Input row
    auto *inputLayout = new QHBoxLayout();
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText("Ask the AI assistant...");
    connect(m_input, &QLineEdit::returnPressed, this, &AiDialog::onSend);
    inputLayout->addWidget(m_input, 1);

    m_sendBtn = new QPushButton("Send", this);
    connect(m_sendBtn, &QPushButton::clicked, this, &AiDialog::onSend);
    inputLayout->addWidget(m_sendBtn);

    m_insertBtn = new QPushButton("Insert Cmd", this);
    m_insertBtn->setToolTip("Insert the last suggested command into the terminal");
    m_insertBtn->setEnabled(false);
    connect(m_insertBtn, &QPushButton::clicked, this, [this]() {
        if (m_lastResponse.isEmpty()) return;

        int stripped = 0;
        const QString cmd =
            AiDialog::extractAndSanitizeCommand(m_lastResponse, &stripped);
        if (cmd.isEmpty()) return;

        // User-facing confirmation — the literal bytes are shown so a
        // prompt-injected LLM response can't silently land an unexpected
        // command in the shell. OWASP LLM01 mitigation.
        //
        // The preview is in a <pre> block so whitespace + control-stripped
        // artifacts are visible verbatim. If the sanitizer stripped any
        // bytes, the user sees "(N bytes were filtered from this command)"
        // — same mechanism as terminalwidget's paste-confirmation.
        // Preview-vs-cmd length parity: if the preview truncates, the
        // user's confirmation doesn't cover the tail — attacker could
        // put benign text first and payload after. Warn explicitly.
        QString preview = cmd.toHtmlEscaped();
        const int kPreviewMax = 500;
        const bool truncatedPreview = (preview.size() > kPreviewMax);
        if (truncatedPreview) {
            preview = preview.left(kPreviewMax) + QStringLiteral("…");
        }
        QString msg = QStringLiteral(
            "The AI suggested this command. It will be typed into the "
            "active terminal if you confirm.<br><br>"
            "<pre style='background:#2b2b2b;color:#eee;padding:8px;"
            "border-radius:4px;white-space:pre-wrap;word-break:break-all;'>"
            "%1</pre>").arg(preview);
        if (truncatedPreview) {
            msg += QStringLiteral(
                "<br><b>⚠ Preview truncated — %1 additional byte(s) "
                "will be executed but are not shown above.</b>"
                ).arg(cmd.size() - kPreviewMax);
        }
        if (stripped > 0) {
            msg += QStringLiteral(
                "<br><i>%1 byte(s) were filtered from this command "
                "(control characters / length cap).</i>").arg(stripped);
        }
        auto reply = QMessageBox::question(
            this, tr("Insert AI-suggested command"), msg,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            emit insertCommand(cmd);
        }
    });
    inputLayout->addWidget(m_insertBtn);
    layout->addLayout(inputLayout);
}

void AiDialog::setTerminalContext(const QString &context) {
    m_terminalContext = context;
}

void AiDialog::setConfig(const QString &endpoint, const QString &apiKey,
                          const QString &model, int contextLines) {
    m_endpoint = endpoint;
    m_apiKey = apiKey;
    m_model = model;
    m_contextLines = contextLines;

    if (m_endpoint.isEmpty()) {
        m_statusLabel->setText("No AI endpoint configured. Set ai_endpoint in config.json");
    } else {
        // 0.6.22 — redact basic-auth credentials before displaying. A user
        // who pasted `https://user:password@host/v1` into ai_endpoint
        // would otherwise see the plaintext password on the status label
        // and in any screenshot they shared. QUrl with PrettyDecoded and
        // the RemoveUserInfo flag strips userinfo cleanly without
        // touching the rest of the URL.
        const QUrl parsed(m_endpoint);
        const QString display = parsed.isValid()
            ? parsed.toString(QUrl::RemoveUserInfo | QUrl::PrettyDecoded)
            : m_endpoint;   // fall back to raw if parse failed
        m_statusLabel->setText("Endpoint: " + display + " | Model: " + m_model);
    }
}

void AiDialog::onSend() {
    QString text = m_input->text().trimmed();
    if (text.isEmpty() || m_endpoint.isEmpty()) return;

    m_input->clear();
    appendMessage("You", text);
    sendRequest(text);
}

void AiDialog::appendMessage(const QString &role, const QString &text) {
    QString html;
    if (role == "You") {
        html = QString("<p><b style='color:#89B4FA;'>You:</b> %1</p>").arg(text.toHtmlEscaped());
    } else if (role == "AI") {
        // Convert markdown code blocks to <pre>
        QString formatted = text.toHtmlEscaped();
        formatted.replace("\n", "<br>");
        html = QString("<p><b style='color:#A6E3A1;'>AI:</b> %1</p>").arg(formatted);
    } else {
        html = QString("<p><i style='color:#F38BA8;'>%1</i></p>").arg(text.toHtmlEscaped());
    }
    m_chatHistory->append(html);
    m_chatHistory->verticalScrollBar()->setValue(m_chatHistory->verticalScrollBar()->maximum());
}

void AiDialog::sendRequest(const QString &userMessage) {
    // Abort any in-flight request to prevent leaked QNetworkReply
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    m_sendBtn->setEnabled(false);
    m_streamBuffer.clear();
    m_sseLineBuffer.clear();
    m_streamTruncated = false;

    QJsonObject systemMsg;
    systemMsg["role"] = "system";
    systemMsg["content"] = QString(
        "You are a helpful terminal assistant. The user is working in a terminal emulator. "
        "Here is the recent terminal output for context:\n\n```\n%1\n```\n\n"
        "Provide concise, actionable answers. When suggesting commands, put them in code blocks."
    ).arg(m_terminalContext);

    QJsonObject userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = userMessage;

    QJsonArray messages;
    messages.append(systemMsg);
    messages.append(userMsg);

    QJsonObject body;
    body["model"] = m_model;
    body["messages"] = messages;
    body["stream"] = true;
    body["max_tokens"] = 1024;

    QUrl url(m_endpoint);
    // Warn once per session if the user has configured an API key against a
    // plaintext HTTP endpoint — the Bearer token would travel in cleartext.
    // Localhost is permitted silently (Ollama/LM Studio default to http://127.0.0.1).
    if (!m_apiKey.isEmpty() && url.scheme() == "http") {
        const QString host = url.host();
        const bool isLocal = (host == "localhost" || host == "127.0.0.1" || host == "::1");
        if (!isLocal && !m_httpWarned) {
            m_httpWarned = true;
            appendMessage("System", "Warning: endpoint uses plaintext HTTP — API key will be sent unencrypted. Prefer https:// for remote providers.");
        }
    }

    QNetworkRequest req{url};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(30000); // 30-second timeout
    if (!m_apiKey.isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());

    m_currentReply = m_netManager.post(req, QJsonDocument(body).toJson());
    connect(m_currentReply, &QNetworkReply::readyRead, this, &AiDialog::onReplyReadyRead);
    connect(m_currentReply, &QNetworkReply::finished, this, &AiDialog::onReplyFinished);
}

void AiDialog::onReplyReadyRead() {
    if (!m_currentReply) return;

    // Append new data to SSE line buffer to handle chunk boundaries correctly
    m_sseLineBuffer += m_currentReply->readAll();

    // Cap buffer to prevent unbounded growth from misbehaving servers
    if (m_sseLineBuffer.size() > 10 * 1024 * 1024) {
        m_sseLineBuffer.clear();
        return;
    }

    // Process complete lines (SSE lines are terminated by \n)
    while (true) {
        int nlPos = m_sseLineBuffer.indexOf('\n');
        if (nlPos < 0) break;

        QString line = QString::fromUtf8(m_sseLineBuffer.left(nlPos)).trimmed();
        m_sseLineBuffer = m_sseLineBuffer.mid(nlPos + 1);

        // Handle both "data: " (standard) and "data:" (some providers)
        if (!line.startsWith("data:")) continue;
        QString json = line.mid(line.startsWith("data: ") ? 6 : 5).trimmed();
        if (json == "[DONE]") continue;

        QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        if (!doc.isObject()) continue;

        QJsonObject obj = doc.object();
        QJsonArray choices = obj["choices"].toArray();
        if (choices.isEmpty()) continue;

        QJsonObject delta = choices[0].toObject()["delta"].toObject();
        QString content = delta["content"].toString();
        if (!content.isEmpty()) {
            // Cap the accumulated response buffer to guard against a
            // misbehaving endpoint streaming unbounded content past
            // max_tokens. m_sseLineBuffer guards the pipe side; this
            // guards the parsed-content side. 10MB mirrors that cap.
            constexpr int kMaxStreamBuffer = 10 * 1024 * 1024;
            if (m_streamBuffer.size() >= kMaxStreamBuffer) {
                if (!m_streamTruncated) {
                    m_streamBuffer += QStringLiteral("\n[response truncated]");
                    m_streamTruncated = true;
                }
                continue;
            }
            m_streamBuffer += content;
        }
    }
}

void AiDialog::onReplyFinished() {
    if (!m_currentReply) return;

    bool hadError = m_currentReply->error() != QNetworkReply::NoError;
    if (hadError) {
        // Try to read non-streaming response (some APIs don't stream)
        QByteArray data = m_currentReply->readAll();
        if (!data.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                QJsonArray choices = obj["choices"].toArray();
                if (!choices.isEmpty()) {
                    QString content = choices[0].toObject()["message"].toObject()["content"].toString();
                    if (!content.isEmpty()) {
                        m_streamBuffer = content;
                        hadError = false; // Valid response despite HTTP error
                    }
                } else if (obj.contains("error")) {
                    appendMessage("Error", obj["error"].toObject()["message"].toString());
                }
            }
        }
        if (hadError && m_streamBuffer.isEmpty()) {
            appendMessage("Error", m_currentReply->errorString());
        } else if (hadError && !m_streamBuffer.isEmpty()) {
            // Had partial stream content before error — mark as incomplete
            m_streamBuffer += "\n[response may be incomplete]";
        }
    }

    if (!m_streamBuffer.isEmpty()) {
        m_lastResponse = m_streamBuffer;
        appendMessage("AI", m_streamBuffer);
        m_insertBtn->setEnabled(true);
    }

    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    m_sendBtn->setEnabled(true);
}

QString AiDialog::extractAndSanitizeCommand(const QString &response,
                                            int *out_stripped) {
    // 1) Extraction — same logic the pre-0.7.12 click handler used.
    QString cmd;
    const int end = response.lastIndexOf(QStringLiteral("```"));
    if (end > 0) {
        const int start = response.lastIndexOf(QStringLiteral("```"), end - 1);
        if (start >= 0 && start < end) {
            cmd = response.mid(start + 3, end - start - 3).trimmed();
            // Strip optional language identifier e.g. "bash\n"
            const int nl = cmd.indexOf('\n');
            if (nl >= 0 && nl < 10) cmd = cmd.mid(nl + 1).trimmed();
        }
    }
    if (cmd.isEmpty()) {
        const QStringList lines = response.split('\n', Qt::SkipEmptyParts);
        if (!lines.isEmpty()) cmd = lines.last().trimmed();
    }
    if (cmd.isEmpty()) {
        if (out_stripped) *out_stripped = 0;
        return QString();
    }

    // 2) Length cap — trim to 4 KiB. Excess bytes count as stripped for
    //    the user-facing counter.
    int stripped = 0;
    if (cmd.size() > kInsertCommandMaxBytes) {
        stripped += cmd.size() - kInsertCommandMaxBytes;
        cmd.truncate(kInsertCommandMaxBytes);
    }

    // 3) Filter dangerous controls — broader set than remotecontrol.cpp
    //    because we also operate on QChar (UTF-16 codepoints), not raw
    //    bytes, so we can strip C1 controls and Unicode attack
    //    codepoints without mangling UTF-8. Strip set:
    //      - C0 minus HT/LF/CR (0x00..0x08, 0x0B..0x1F)
    //      - DEL (0x7F)
    //      - C1 controls (0x80..0x9F) — NEL U+0085 et al, line-terminate
    //        in some shells
    //      - Line/paragraph separators U+2028, U+2029 — ditto
    //      - Bidi overrides U+202A..U+202E and U+2066..U+2069 —
    //        "Trojan Source" class (CVE-2021-42574) — these can make
    //        the confirmation-dialog preview display different text
    //        from what actually executes
    //      - Zero-width codepoints U+200B..U+200D, U+FEFF — can hide
    //        tokens in the preview (e.g. `rm<ZWSP>` reads as `rm`)
    //    0.7.12 /indie-review re-review expansion.
    auto isDangerous = [](ushort u) -> bool {
        const bool isAllowedWs = (u == 0x09 || u == 0x0A || u == 0x0D);
        if (u < 0x20 && !isAllowedWs) return true;   // C0
        if (u == 0x7F) return true;                  // DEL
        if (u >= 0x80 && u <= 0x9F) return true;     // C1 incl. NEL
        if (u == 0x2028 || u == 0x2029) return true; // LS / PS
        if (u >= 0x202A && u <= 0x202E) return true; // bidi overrides (old)
        if (u >= 0x2066 && u <= 0x2069) return true; // bidi isolates (new)
        if (u >= 0x200B && u <= 0x200D) return true; // ZWSP / ZWNJ / ZWJ
        if (u == 0xFEFF) return true;                // ZWNBSP / BOM-as-ZWSP
        return false;
    };

    QString clean;
    clean.reserve(cmd.size());
    for (QChar c : cmd) {
        if (isDangerous(c.unicode())) {
            ++stripped;
            continue;
        }
        clean.append(c);
    }

    if (out_stripped) *out_stripped = stripped;
    return clean;
}
