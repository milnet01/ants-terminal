#pragma once

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QLabel>

// AI Assistant dialog — sends terminal context to an OpenAI-compatible API
class AiDialog : public QDialog {
    Q_OBJECT

public:
    explicit AiDialog(QWidget *parent = nullptr);

    void setTerminalContext(const QString &context);
    void setConfig(const QString &endpoint, const QString &apiKey,
                   const QString &model, int contextLines);

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
    // of control bytes removed + the number of bytes truncated by the
    // 4 KiB cap (same counter, the UX just tells the user "X bytes
    // were filtered").
    static QString extractAndSanitizeCommand(const QString &response,
                                             int *out_stripped = nullptr);

    static constexpr int kInsertCommandMaxBytes = 4096;

private slots:
    void onSend();
    void onReplyFinished();
    void onReplyReadyRead();

private:
    void appendMessage(const QString &role, const QString &text);
    void sendRequest(const QString &userMessage);

    QTextEdit *m_chatHistory = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_insertBtn = nullptr;
    QLabel *m_statusLabel = nullptr;

    QNetworkAccessManager m_netManager;
    QNetworkReply *m_currentReply = nullptr;

    QString m_terminalContext;
    QString m_endpoint;
    QString m_apiKey;
    QString m_model;
    int m_contextLines = 50;
    QString m_lastResponse;   // Last complete AI response (for insert)
    QString m_streamBuffer;   // Accumulates streaming response
    QByteArray m_sseLineBuffer; // Buffers incomplete SSE lines across TCP chunks
    bool m_httpWarned = false;  // Show plaintext-HTTP warning once per dialog instance
    bool m_streamTruncated = false; // Marker appended once when m_streamBuffer hits cap

public:
    // Abort any in-flight reply before members start destructing. Without
    // this, a reply arriving during the narrow window of member destruction
    // can deliver readyRead/finished to a partially-destroyed AiDialog.
    ~AiDialog() override;
};
