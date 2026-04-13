#pragma once

#include <QObject>
#include <QTimer>
#include <QProcessEnvironment>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileSystemWatcher>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDateTime>

class TerminalWidget;

// Metadata for a single Claude Code session
struct ClaudeSession {
    QString sessionId;
    QString projectPath;     // decoded real path (e.g. /mnt/Storage/Scripts/Linux/Ants)
    QString projectEncoded;  // encoded dir name (e.g. -mnt-Storage-Scripts-Linux-Ants)
    QString transcriptPath;  // full path to .jsonl file
    QString name;            // session name (from metadata, if any)
    QString firstMessage;    // first user message (summary)
    QDateTime lastModified;
    qint64 sizeBytes = 0;
    bool isActive = false;   // currently running
};

// A project with its sessions
struct ClaudeProject {
    QString path;            // decoded real path
    QString encodedName;     // encoded dir name
    QString memorySnippet;   // first few lines of MEMORY.md
    QList<ClaudeSession> sessions;
    QDateTime lastActivity;  // most recent session date
};

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

    // MCP server for terminal capabilities
    bool startMcpServer(const QString &socketPath);
    void stopMcpServer();
    void setScrollbackProvider(std::function<QString(int)> provider);
    void setCwdProvider(std::function<QString()> provider);
    void setLastCommandProvider(std::function<QPair<int,QString>()> provider);
    void setGitStatusProvider(std::function<QString()> provider);
    void setEnvironmentProvider(std::function<QString()> provider);

    // Project/session discovery
    QList<ClaudeProject> discoverProjects() const;
    QString sessionSummary(const QString &transcriptPath) const;
    QString projectMemory(const QString &projectEncoded) const;
    static QString decodeProjectPath(const QString &encoded);
    static QString encodeProjectPath(const QString &path);

    // Environment setup
    static QProcessEnvironment claudeEnv();

    // Auto-configure hooks in .claude/settings.local.json
    static void ensureHooksConfigured(const QString &projectDir, int port);

    // Terminal output notification — call when PTY output is received while
    // Claude is running so status detection can supplement transcript parsing
    void notifyTerminalOutput();

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
    std::function<QPair<int,QString>()> m_lastCommandProvider;
    std::function<QString()> m_gitStatusProvider;
    std::function<QString()> m_envProvider;

    // Session tracking
    QString m_activeSessionId;
    QString m_transcriptPath;
    QFileSystemWatcher m_transcriptWatcher;
};
