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

    // ANTS-1168: scope the session list to a single project root so a
    // dialog opened from tab A doesn't show tab B's most-recent
    // session (the global-newest-by-mtime trap of the ANTS-1163
    // family). Empty string falls back to the unscoped global list.
    void setProjectFilter(const QString &projectCwd);

private slots:
    void onSessionSelected(int index);
    void onRefresh();

private:
    void loadTranscript(const QString &path);
    QString formatEntry(const QJsonObject &entry) const;

    ClaudeIntegration *m_integration;
    QComboBox *m_sessionCombo = nullptr;
    QTextEdit *m_transcriptView = nullptr;
    QString m_projectFilter;
};
