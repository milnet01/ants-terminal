#pragma once

#include <QDialog>
#include <QTextEdit>
#include <QComboBox>
#include <QJsonArray>

class ClaudeIntegration;

// Dialog for viewing Claude Code session transcripts
class ClaudeTranscriptDialog : public QDialog {
    Q_OBJECT

public:
    explicit ClaudeTranscriptDialog(ClaudeIntegration *integration,
                                     QWidget *parent = nullptr);

private slots:
    void onSessionSelected(int index);
    void onRefresh();

private:
    void loadTranscript(const QString &path);
    QString formatEntry(const QJsonObject &entry) const;

    ClaudeIntegration *m_integration;
    QComboBox *m_sessionCombo = nullptr;
    QTextEdit *m_transcriptView = nullptr;
};
