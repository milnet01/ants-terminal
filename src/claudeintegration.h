#pragma once

#include <QObject>
#include <QTimer>
#include <QLabel>
#include <QProcess>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileSystemWatcher>
#include <QLocalServer>
#include <QLocalSocket>

class TerminalWidget;

// Claude Code state detected from process inspection + hooks
enum class ClaudeState {
    NotRunning,
    Idle,
    Thinking,
    ToolUse,
};

// Comprehensive Claude Code integration for Ants Terminal
class ClaudeIntegration : public QObject {
    Q_OBJECT

public:
    explicit ClaudeIntegration(QObject *parent = nullptr);
    ~ClaudeIntegration() override;

    // Process detection: check if Claude Code is running under given shell PID
    void setShellPid(pid_t pid);
    ClaudeState currentState() const { return m_state; }
    QString currentTool() const { return m_currentTool; }
    int contextPercent() const { return m_contextPercent; }

    // Session transcript
    QString activeSessionPath() const;
    QJsonArray loadTranscript(const QString &path) const;
    QStringList recentSessions() const;

    // Hook server (receives events from Claude Code hooks)
    bool startHookServer();
    void stopHookServer();
    int hookServerPort() const;

    // MCP server for terminal capabilities
    bool startMcpServer(const QString &socketPath);
    void stopMcpServer();
    void setScrollbackProvider(std::function<QString(int)> provider);
    void setCwdProvider(std::function<QString()> provider);

    // File change tracking from transcript
    QStringList recentlyChangedFiles() const { return m_changedFiles; }

    // Environment setup
    static QProcessEnvironment claudeEnv();

    // Auto-configure hooks in .claude/settings.local.json
    static void ensureHooksConfigured(const QString &projectDir, int port);

signals:
    void stateChanged(ClaudeState state, const QString &detail);
    void toolStarted(const QString &toolName, const QString &input);
    void toolFinished(const QString &toolName, bool success);
    void sessionStarted(const QString &sessionId);
    void sessionStopped(const QString &reason);
    void fileChanged(const QString &filePath);
    void contextUpdated(int percent);
    void permissionRequested(const QString &tool, const QString &input);

private slots:
    void pollClaudeProcess();
    void onHookConnection();
    void onMcpConnection();

private:
    void processHookEvent(const QJsonObject &event);
    void parseTranscriptForState(const QString &path);
    void updateChangedFiles(const QJsonObject &event);

    pid_t m_shellPid = 0;
    ClaudeState m_state = ClaudeState::NotRunning;
    QString m_currentTool;
    int m_contextPercent = 0;
    QStringList m_changedFiles;

    // Process polling
    QTimer m_pollTimer;
    pid_t m_claudePid = 0;

    // Hook server
    QLocalServer *m_hookServer = nullptr;

    // MCP server
    QLocalServer *m_mcpServer = nullptr;
    std::function<QString(int)> m_scrollbackProvider;
    std::function<QString()> m_cwdProvider;

    // Session tracking
    QString m_activeSessionId;
    QFileSystemWatcher m_transcriptWatcher;
};
