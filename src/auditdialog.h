#pragma once

#include <QDialog>
#include <QTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QProcess>
#include <QTimer>
#include <QTemporaryFile>

class ToggleSwitch;

struct AuditCheck {
    QString id;
    QString name;
    QString description;
    QString category;
    QString command;        // shell command to run
    bool autoSelect = false;
    bool available = true;
    ToggleSwitch *toggle = nullptr;
};

class AuditDialog : public QDialog {
    Q_OBJECT

public:
    explicit AuditDialog(const QString &projectPath, QWidget *parent = nullptr);

signals:
    // Emitted when user clicks "Review with Claude" — carries path to temp results file
    void reviewRequested(const QString &resultsFile);

private:
    void detectProject();
    void populateChecks();
    void buildUI();
    void runAudit();
    void runNextCheck();
    void onCheckFinished(int exitCode, QProcess::ExitStatus status);
    void appendResult(const QString &title, const QString &output, bool isWarning = false);
    bool toolExists(const QString &tool);

    QString m_projectPath;
    QStringList m_detectedTypes;

    QList<AuditCheck> m_checks;
    int m_currentCheck = -1;
    int m_checksRun = 0;
    int m_totalSelected = 0;

    QLabel *m_pathLabel = nullptr;
    QLabel *m_typesLabel = nullptr;
    QPushButton *m_runBtn = nullptr;
    QProgressBar *m_progress = nullptr;
    QTextEdit *m_results = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProcess *m_process = nullptr;
    QTimer *m_timeout = nullptr;

    QPushButton *m_reviewBtn = nullptr;
    QString plainTextResults() const;
};
