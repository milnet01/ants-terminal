#pragma once

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>

#include "llmclient.h"

// AI Assistant dialog — sends terminal context to an OpenAI-compatible API.
// ANTS-1727: the network mechanics (request build, SSE drain, 10 MiB caps,
// scheme allowlist, transfer timeout) are delegated to an owned LlmClient.
// The dialog keeps its chat UI, the OWASP LLM06 scrub + user-facing
// redaction notice (UX-coupled), and the Insert-Cmd confirmation flow.
class AiDialog : public QDialog {
    Q_OBJECT

public:
    explicit AiDialog(QWidget *parent = nullptr);

    void setTerminalContext(const QString &context);
    void setConfig(const QString &endpoint, const QString &apiKey,
                   const QString &model, int contextLines);

    // ANTS-1168: clear transient state (status label, input field,
    // any in-flight reply) on each show() so a stale "Error: rate
    // limited" or half-typed prompt from the previous open doesn't
    // surface in this open. Chat history is preserved deliberately
    // — users expect the conversation to continue across re-opens.
    void resetTransient();

signals:
    void insertCommand(const QString &cmd); // Insert AI-suggested command into PTY

public:
    // Extract a candidate command from an AI response (fenced ``` block
    // preferred, else last non-empty line), strip dangerous control
    // chars (C0 except HT/LF/CR + DEL — same set remotecontrol.cpp
    // filters), and cap at 4 KiB. Defense against OWASP LLM02 —
    // an LLM coaxed into emitting ESC sequences or NUL bytes can
    // otherwise reprogram the terminal the moment the user clicks
    // Insert. Exposed as a static helper so feature tests can verify
    // the sanitization without a full dialog round-trip.
    //
    // Returns the sanitized command (possibly empty if no candidate
    // was found). If `out_stripped` is non-null, receives the number
    // of control codepoints removed + the number of chars truncated by
    // the kInsertCommandMaxChars cap (same counter, the UX just tells
    // the user "X bytes were filtered").
    static QString extractAndSanitizeCommand(const QString &response,
                                             int *out_stripped = nullptr);

    // Length cap in QString chars (UTF-16 units), NOT bytes — the
    // sanitizer is codepoint-oriented (it strips C1 / Unicode-attack
    // codepoints on QChar), so the cap is a char count (ANTS-2205).
    static constexpr int kInsertCommandMaxChars = 4096;

private slots:
    void onSend();
    void onLlmFinished(const LlmResult &result);

private:
    void appendMessage(const QString &role, const QString &text);
    void sendRequest(const QString &userMessage);

    QTextEdit *m_chatHistory = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_insertBtn = nullptr;
    QLabel *m_statusLabel = nullptr;

    LlmClient *m_client = nullptr;

    QString m_terminalContext;
    QString m_endpoint;
    QString m_apiKey;
    QString m_model;
    int m_contextLines = 50;
    QString m_lastResponse;   // Last complete AI response (for insert)

public:
    ~AiDialog() override;
};
