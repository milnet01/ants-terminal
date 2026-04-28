#pragma once

#include <QObject>
#include <QTimer>
#include <QProcessEnvironment>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDateTime>

class TerminalWidget;

// Metadata for a single Claude Code session
struct ClaudeSession {
    QString sessionId;
    QString projectPath;     // decoded real path (e.g. $HOME/projects/myapp)
    QString projectEncoded;  // encoded dir name (e.g. -home-user-projects-myapp)
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
    Compacting,   // /compact in flight — detected from transcript or PreCompact hook
};

// Result of parsing a transcript tail. Populated by
// ClaudeIntegration::parseTranscriptTail — a pure helper shared by the
// active-tab ClaudeIntegration (which emits stateChanged etc.) and the
// per-tab ClaudeTabTracker (which stores one of these per tab without
// any emit).
struct ClaudeTranscriptSnapshot {
    bool hasEvents = false;        // true iff tail parsed at least one event
    bool stateDetermined = false;  // true iff last non-metadata event was recognized
    ClaudeState state = ClaudeState::NotRunning;
    QString tool;                  // non-empty iff state == ToolUse
    QString detail;                // short human-readable label ("thinking", "Bash", …)
    int contextPercent = -1;       // -1 if no usage.input_tokens observed in window
    bool planMode = false;         // result of the latch + most-recent permission-mode
    bool auditing = false;         // /audit turn in flight
    QJsonObject toolUseBlock;      // raw tool_use block for updateChangedFiles; empty if N/A
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
    const QString &currentTool() const { return m_currentTool; }
    int contextPercent() const { return m_contextPercent; }
    bool planMode() const { return m_planMode; }

    // session_id from the most recent hook event. Updated before any
    // signal is emitted by processHookEvent, so handlers of those
    // signals (e.g. permissionRequested) can read it to route the event
    // to the correct per-shell tracker entry in multi-Claude layouts.
    // Empty if no hook has fired yet.
    const QString &lastHookSessionId() const { return m_lastHookSessionId; }

    // Session transcript. `projectCwd` scopes the lookup to the
    // Claude project directory matching the caller's working tree —
    // walks up `projectCwd` and checks each ancestor's encoded form
    // against `~/.claude/projects/<encoded>/`, returning the newest
    // `*.jsonl` from the deepest match. Empty `projectCwd` falls
    // back to the global newest, matching the pre-0.7.44 behavior
    // (kept for callers that genuinely want the system-wide newest).
    QString activeSessionPath(const QString &projectCwd = QString()) const;
    // Project-scoped form of the lookup, exposed as a free static so
    // ClaudeTabTracker (and any future caller without a ClaudeIntegration
    // handle) can resolve a per-shell transcript without drilling into
    // private state. Same walk-up semantics as activeSessionPath's
    // non-empty branch — returns empty if no ancestor has an encoded
    // project directory under `~/.claude/projects/`.
    static QString sessionPathForCwd(const QString &projectCwd);
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

    // Exposed for tests/features/claude_status_bar/. Driving the transcript
    // parser directly with a synthetic .jsonl file is how the feature test
    // exercises the full (last-event → ClaudeState) mapping without
    // spawning a real Claude Code process. Safe to call from production
    // code too (the file-watcher path uses it directly).
    void parseTranscriptForState(const QString &path);

    // Pure tail parser. Reads a ~32 KB (growing up to 4 MiB) suffix of the
    // transcript file and derives the Claude Code state the tail implies.
    // latchedPlanMode is the caller's current plan-mode state: plan mode
    // persists across many turns, so if the tail window doesn't contain
    // any permission-mode event we retain the latch rather than reset.
    // Stateless — safe to call concurrently from multiple trackers.
    static ClaudeTranscriptSnapshot parseTranscriptTail(
        const QString &path, bool latchedPlanMode);

signals:
    void stateChanged(ClaudeState state, const QString &detail);
    void toolStarted(const QString &toolName, const QString &input);
    void toolFinished(const QString &toolName, bool success);
    void sessionStarted(const QString &sessionId);
    void sessionStopped(const QString &reason);
    void fileChanged(const QString &filePath);
    void contextUpdated(int percent);
    void permissionRequested(const QString &tool, const QString &input);
    // Plan mode is orthogonal to tool-use state — the user toggles it
    // with Shift+Tab in the Claude Code TUI. Detected from transcript
    // `permission-mode` metadata events (mode == "plan" / "plan_mode").
    // Surfaced separately so the status bar can show "Claude: planning"
    // even while Claude is running Read/Grep tools in plan mode.
    void planModeChanged(bool active);
    // Auditing: user invoked the /audit skill in the most recent user
    // message. Detected via the same transcript-scan shape as /compact.
    // Separate signal because auditing spans many tool-use turns and
    // needs its own status-bar label.
    void auditingChanged(bool active);

private slots:
    void pollClaudeProcess();
    void onHookConnection();
    void onMcpConnection();

private:
    void processHookEvent(const QJsonObject &event);
    // parseTranscriptForState is declared above (public, for tests).
    void updateChangedFiles(const QJsonObject &event);

    pid_t m_shellPid = 0;
    ClaudeState m_state = ClaudeState::NotRunning;
    QString m_currentTool;
    int m_contextPercent = 0;
    QStringList m_changedFiles;
    bool m_planMode = false;
    bool m_auditing = false;
    // 0.7.54 (2026-04-27 indie-review) — per-shellPid plan-mode cache.
    // setShellPid(pid) used to reset m_planMode unconditionally on tab
    // switch, then rely on the next pollClaudeProcess parse to re-derive
    // it. If the new tab's transcript-tail window didn't include the
    // permission-mode toggle event (event scrolled past the tail
    // bound), m_planMode silently stayed false and the bottom status
    // dropped the "plan mode" indicator until the user toggled again.
    // Cache lets us restore the latched state across tab flips.
    QHash<pid_t, bool> m_planModeByPid;
    // session_id from the most recent hook event. Read by handlers via
    // lastHookSessionId() — see that accessor's comment.
    QString m_lastHookSessionId;

    // Process polling
    QTimer m_pollTimer;
    pid_t m_claudePid = 0;

    // Transcript parsing is event-driven (QFileSystemWatcher -> inotify) with a
    // debounce timer to coalesce streaming-output bursts, plus a slow backstop
    // tick counter that forces a re-parse every N poll cycles in case the
    // watcher missed a file-replaced event.
    QTimer m_transcriptDebounce;
    int m_transcriptBackstopTicks = 0;

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
