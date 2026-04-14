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
