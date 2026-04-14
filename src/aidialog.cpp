#include "aidialog.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QScrollBar>

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
        if (!m_lastResponse.isEmpty()) {
            // Extract code blocks or the last line that looks like a command
            QString cmd;
            // Find the last complete ```...``` code block
            int end = m_lastResponse.lastIndexOf("```");
            if (end > 0) {
                int start = m_lastResponse.lastIndexOf("```", end - 1);
                if (start >= 0 && start < end) {
                    cmd = m_lastResponse.mid(start + 3, end - start - 3).trimmed();
                    // Remove language identifier (e.g., "bash\n")
                    int nl = cmd.indexOf('\n');
                    if (nl >= 0 && nl < 10) cmd = cmd.mid(nl + 1).trimmed();
                }
            }
            if (cmd.isEmpty()) {
                // Use the last non-empty line
                QStringList lines = m_lastResponse.split('\n', Qt::SkipEmptyParts);
                if (!lines.isEmpty()) cmd = lines.last().trimmed();
            }
            if (!cmd.isEmpty())
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
        m_statusLabel->setText("Endpoint: " + m_endpoint + " | Model: " + m_model);
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
