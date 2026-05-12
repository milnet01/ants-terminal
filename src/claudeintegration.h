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

    // ANTS-1131 — prune the per-PID plan-mode cache when a tab closes.
    // Without this, m_planModeByPid grows monotonically over a long
    // session and Linux PID reuse can poison a fresh shell with a
    // stale plan-mode flag from a closed Claude tab. MainWindow::closeTab
    // calls this alongside m_claudeTabTracker->untrackShell(pid).
    void forgetShell(pid_t pid);
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
    //
    // ANTS-1163 (2026-05-07) — two-layer freshness filter:
    //
    //   * `minLastEventMs` (process-anchored identity): when > 0, drop
    //     candidate JSONLs whose effective last-event ms is < this
    //     boundary. Pass `processStartTimeMs(claudePid)` here.
    //   * `nowMs` (24h liveness floor): when > 0, drop candidate
    //     JSONLs whose effective last-event ms is < `nowMs - 24h`.
    //
    // "Effective last-event ms" is `lastEventTimestampMs(path)` if it
    // returns > 0, else `QFileInfo::lastModified()`. Surviving
    // candidate with the largest effective ms wins.
    //
    // Both default to 0 → legacy newest-by-mtime behaviour.
    static QString sessionPathForCwd(const QString &projectCwd,
                                      qint64 minLastEventMs = 0,
                                      qint64 nowMs = 0);

    // Wall-clock epoch ms when `pid` started, derived from
    // /proc/<pid>/stat field 22 (starttime in clock ticks since boot)
    // and /proc/stat's `btime` (boot time epoch). Returns 0 if the
    // PID is dead, /proc isn't mounted, or parsing fails. Stateless,
    // safe to call from any thread.
    static qint64 processStartTimeMs(pid_t pid);

    // Most recent ISO 8601 `timestamp` field in the JSONL tail at
    // `path`. Walks backwards from EOF over the last 32 KB, parsing
    // each line as JSON, returning the first `timestamp` that parses
    // successfully (skipping metadata events like `last-prompt`,
    // `permission-mode`, `file-history-snapshot`, `ai-title` that
    // have no `timestamp` field). Returns 0 if no timestamped event
    // is found in the tail window. Used by sessionPathForCwd to
    // anchor freshness against transcript content rather than mtime.
    static qint64 lastEventTimestampMs(const QString &path);

    // Walk the children of `shellPid` via /proc/<pid>/task/<pid>/children
    // and return the pid of the first child that looks like a Claude Code
    // process — argv[0] basename matches `claude` / `claude-code`, OR a
    // node/deno/bun launcher with a `claude`/`claude-code` script in
    // argv[1..]. Returns 0 if no such child exists or if /proc isn't
    // mounted. Stateless, safe to call from any thread.
    //
    // 0.7.57 (2026-04-30 indie-review ANTS-1048) — extracted from
    // ClaudeIntegration::pollClaudeProcess and
    // ClaudeTabTracker::detectClaudeChild, which carried two near-
    // identical copies of this walk. Rule of three (two near-identical
    // copies plus the obvious next caller — the planned local-subagent
    // framework) says extract now.
    static pid_t findClaudeChildPid(pid_t shellPid);

    QJsonArray loadTranscript(const QString &path) const;
    QStringList recentSessions() const;
    // ANTS-1168: project-scoped variant. When projectCwd is non-empty,
    // restrict the result to the encoded project directory matching
    // that path (or any of its ancestors); otherwise behave like the
    // unscoped recentSessions(). Used by the transcript dialog so a
    // dialog shown from tab A doesn't surface tab B's most-recently-
    // touched session.
    QStringList recentSessionsForCwd(const QString &projectCwd) const;

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

    // ANTS-1244: surface the read-only remote-control verbs as MCP
    // tools. Each provider returns the IPC verb's compact-JSON
    // response as a string; the dispatcher wraps it in a text
    // content block. `setGetTextProvider`'s `tab=-1` means "active
    // tab", `lines=0` means "default 100" — see spec § 2.d.
    // ANTS-1247: widened to thread an optional status filter
    // ("all"/"active"/"shipped") to cmdRoadmapQuery. Empty string =
    // "all" (back-compat).
    void setRoadmapQueryProvider(std::function<QString(const QString&)> provider);
    void setTabListProvider(std::function<QString()> provider);
    void setGetTextProvider(std::function<QString(int,int)> provider);

    // ANTS-1248: workspace_search provider — ripgrep-backed code
    // search. Full-QJsonObject signature (matches the cmdGetText
    // widening idiom from ANTS-1244) so future schema additions
    // don't require new setter overloads.
    void setWorkspaceSearchProvider(std::function<QString(const QJsonObject&)> provider);

    // ANTS-1249: file_outline provider — regex scanner over a file,
    // returns header_doc + symbols[]. Same full-QJsonObject shape.
    void setFileOutlineProvider(std::function<QString(const QJsonObject&)> provider);

    // ANTS-1250: git_state provider — consolidated git tool (status /
    // log / diff via op discriminator). Same full-QJsonObject shape.
    void setGitStateProvider(std::function<QString(const QJsonObject&)> provider);

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

    // Exposed for tests/features/claude_status_bar_per_tab/. Same pattern
    // as parseTranscriptForState — drives the hook dispatch that
    // onHookConnection feeds in production, so the per-tab session-id
    // gate (ANTS-1161) can be exercised without standing up a real UDS.
    void processHookEventForTest(const QJsonObject &event) {
        processHookEvent(event);
    }
    // Exposed so the same test can seed a focused-tab transcript path
    // without staging a real Claude process under /proc.
    void setTranscriptPathForTest(const QString &path) {
        m_transcriptPath = path;
    }

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
    // ANTS-1161 — gate hook events on the focused tab's session.
    // The hook server is one UDS shared across every Claude under any
    // tab, so without this gate Tab B's PreToolUse would clobber the
    // singleton's m_state/m_currentTool and the bottom Claude: <state>
    // widget would paint Tab B's tool name on Tab A's focused row.
    // Returns true when `sessionId` belongs to the focused tab —
    // matches the basename of m_transcriptPath against `sessionId`
    // (Claude Code stores transcripts as `<session-uuid>.jsonl`,
    // mirroring ClaudeTabTracker::shellForSessionId). Tolerant of
    // pre-poll state: empty sessionId or empty m_transcriptPath
    // returns true so first-event behaviour is unchanged.
    bool isFocusedTabSession(const QString &sessionId) const;

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
    // ANTS-1244 — delegate-into-RemoteControl providers.
    // ANTS-1247: m_roadmapQueryProvider widened to accept status filter.
    std::function<QString(const QString&)> m_roadmapQueryProvider;
    std::function<QString()> m_tabListProvider;
    std::function<QString(int,int)> m_getTextProvider;
    // ANTS-1248: workspace_search provider — full-QJsonObject shape.
    std::function<QString(const QJsonObject&)> m_workspaceSearchProvider;
    // ANTS-1249: file_outline provider — full-QJsonObject shape.
    std::function<QString(const QJsonObject&)> m_fileOutlineProvider;
    // ANTS-1250: git_state provider — full-QJsonObject shape.
    std::function<QString(const QJsonObject&)> m_gitStateProvider;

    // Session tracking
    QString m_activeSessionId;
    QString m_transcriptPath;
    QFileSystemWatcher m_transcriptWatcher;
};
