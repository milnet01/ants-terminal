#pragma once

#include <QDialog>
#include <QTreeWidget>
#include <QTextEdit>
#include <QSplitter>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>

class ClaudeIntegration;
class Config;
class TerminalWidget;
struct ClaudeProject;
struct ClaudeSession;

// Dialog for browsing Claude Code projects and resuming sessions
class ClaudeProjectsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ClaudeProjectsDialog(ClaudeIntegration *integration,
                                   Config *config,
                                   QWidget *parent = nullptr);

    void refresh();

signals:
    // Emitted when user wants to resume a session in the terminal
    void resumeSession(const QString &projectPath, const QString &sessionId, bool fork);
    // Emitted when user wants to continue the latest session in a project
    void continueProject(const QString &projectPath);
    // Emitted when user wants to start a new session in a project/directory
    void newSession(const QString &projectPath);

private slots:
    void onProjectSelected();
    void onSessionSelected();
    void onSessionDoubleClicked(QTreeWidgetItem *item, int column);
    void onResume();
    void onContinue();
    void onForkResume();
    void onNewSession();
    void onNewProjectInDir();
    void onAddProjectDir();
    void onRemoveProjectDir();

private:
    void populateProjects();
    void populateSessions(int projectIndex);
    void showSessionPreview(const ClaudeSession &session);
    void refreshProjectDirCombo();
    QString formatSize(qint64 bytes) const;
    QString formatTimeAgo(const QDateTime &dt) const;

    ClaudeIntegration *m_integration;
    Config *m_config;
    QList<ClaudeProject> m_projects;

    // UI
    QTreeWidget *m_projectTree = nullptr;
    QTreeWidget *m_sessionTree = nullptr;
    QTextEdit *m_previewPane = nullptr;
    QLabel *m_memoryLabel = nullptr;
    QPushButton *m_resumeBtn = nullptr;
    QPushButton *m_continueBtn = nullptr;
    QPushButton *m_forkBtn = nullptr;
    QPushButton *m_newBtn = nullptr;
    QComboBox *m_projectDirCombo = nullptr;
};
