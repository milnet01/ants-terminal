#include "aidialog.h"
#include "dialogchrome.h"
#include "secretredact.h"

#include <QMessageBox>
#include <QScrollBar>
#include <QUrl>

AiDialog::~AiDialog() {
    // m_client (a child QObject) aborts its in-flight reply in its own
    // destructor; abort explicitly first to close the narrow window where
    // a late finished() could fire during member teardown.
    if (m_client) m_client->abort();
}

AiDialog::AiDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("AI Assistant");
    setMinimumSize(500, 400);
    resize(600, 500);

    m_client = new LlmClient(this);
    connect(m_client, &LlmClient::finished, this, &AiDialog::onLlmFinished);

    // ANTS-1242 — frameless + theme-aware TitleBar.
    auto chrome = DialogChrome::install(this);
    QWidget *content = chrome.contentArea;

    auto *layout = new QVBoxLayout(content);
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

void AiDialog::resetTransient() {
    // ANTS-1168: clear per-open transient state. Chat history is left
    // alone deliberately — users expect the conversation thread to
    // continue across re-opens.
    if (m_input)        m_input->clear();
    if (m_statusLabel)  m_statusLabel->clear();
    if (m_client)       m_client->abort();
}

void AiDialog::setConfig(const QString &endpoint, const QString &apiKey,
                          const QString &model, int contextLines) {
    // 0.7.52 (2026-04-27 indie-review HIGH) — scheme allowlist on the
    // configured AI endpoint. The dialog routes user prompts + terminal
    // context (potentially carrying secrets, file paths, command output)
    // to whatever URL is here. A `file://` / `gopher://` / bare-host
    // schema with no transport encryption would leak that traffic, so
    // reject anything other than http/https up-front rather than at
    // request-time when the body is already serialised. Empty endpoint
    // still means "AI disabled" (handled below); we only validate when
    // a non-empty value is supplied.
    if (!endpoint.isEmpty()) {
        QString scheme;
        if (!LlmClient::isEndpointAllowed(endpoint, &scheme)) {
            m_endpoint.clear();
            m_apiKey.clear();
            m_model = model;
            m_contextLines = contextLines;
            m_statusLabel->setText(
                QStringLiteral("AI endpoint rejected — only http/https are "
                               "permitted (got '%1'). Set ai_endpoint in "
                               "config.json to a https://… URL.").arg(scheme));
            return;
        }
    }

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
    m_client->abort();
    m_sendBtn->setEnabled(false);

    // OWASP LLM06: scrub well-known secret shapes out of both the
    // scrollback context and the user's own message before either
    // leaves the process. Terminal output may contain `cat .env`,
    // `aws configure show`, a `git clone https://ghp_…@…` URL, or a
    // freshly-pasted SSH private key. The user's question may also
    // contain pasted secrets. Both strings are scrubbed; the chat
    // history display (appendMessage("You", …) in onSend) still shows
    // the pre-redaction text — redaction is a network-boundary
    // concern, not a UX one. Contract:
    // tests/features/ai_context_redaction/spec.md.
    const auto scrubbedContext = SecretRedact::scrub(m_terminalContext);
    const auto scrubbedUser    = SecretRedact::scrub(userMessage);

    const QString systemPrompt = QString(
        "You are a helpful terminal assistant. The user is working in a terminal emulator. "
        "Here is the recent terminal output for context:\n\n```\n%1\n```\n\n"
        "Provide concise, actionable answers. When suggesting commands, put them in code blocks."
    ).arg(scrubbedContext.text);

    const int totalRedacted = scrubbedContext.redactedCount + scrubbedUser.redactedCount;
    if (totalRedacted > 0) {
        // Tell the user the payload differs from what they saw/typed so
        // they don't wonder why the AI's answer doesn't match their
        // terminal state. Singular/plural kept simple — this surface is
        // log-like, not copy-polished.
        appendMessage(QStringLiteral("System"),
                      QString("Note: %1 secret%2 redacted from outbound request "
                              "(OWASP LLM06 — see tests/features/ai_context_redaction/spec.md).")
                          .arg(totalRedacted)
                          .arg(totalRedacted == 1 ? "" : "s"));
    }

    // ANTS-2108 — refuse (not just warn) when an API key would travel in
    // cleartext to a remote host. LlmClient::send() enforces this as a
    // backstop, but short-circuit here so the user gets one clear message
    // instead of a generic refusal echoed back through onLlmFinished.
    // Localhost is exempt (Ollama/LM Studio default to http://127.0.0.1).
    if (!m_apiKey.isEmpty() && LlmClient::isPlaintextRemote(m_endpoint)) {
        appendMessage("System", "Refused: endpoint is plaintext HTTP to a remote host — "
                                "the API key would travel unencrypted. Use https:// "
                                "(localhost is exempt).");
        return;
    }

    // Prompts are already scrubbed above (UX-coupled notice), so the
    // client doesn't re-scrub — scrubSecrets=false avoids double work.
    LlmRequest req;
    req.endpoint = m_endpoint;
    req.apiKey = m_apiKey;
    req.model = m_model;
    req.systemPrompt = systemPrompt;
    req.userPrompt = scrubbedUser.text;
    req.maxTokens = 1024;
    req.timeoutMs = 30000;
    req.scrubSecrets = false;
    m_client->send(req);
}

void AiDialog::onLlmFinished(const LlmResult &result) {
    if (!result.text.isEmpty()) {
        m_lastResponse = result.text;
        appendMessage("AI", result.text);
        // ANTS-1144 — fail-closed on partial-stream errors. Only enable
        // Insert when the response landed cleanly; a truncated response
        // (network drop mid-fenced-block) would otherwise invite the user
        // to confirm a command missing its tail.
        m_insertBtn->setEnabled(result.ok);
    } else if (!result.error.isEmpty()) {
        appendMessage("Error", result.error);
    }

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
            // Strip optional language identifier e.g. "bash\n". Language
            // IDs are single unbroken tokens; a first line with spaces
            // or tabs is a short command, not a language hint, and must
            // not be eaten. (0.7.13 — /indie-review 2026-04-23.)
            const int nl = cmd.indexOf('\n');
            if (nl > 0 && nl < 10) {
                const QStringView first = QStringView(cmd).left(nl);
                if (!first.contains(QLatin1Char(' '))
                    && !first.contains(QLatin1Char('\t'))) {
                    cmd = cmd.mid(nl + 1).trimmed();
                }
            }
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
