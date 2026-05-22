#pragma once

#include <QObject>
#include <QTimer>
#include <QProcessEnvironment>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QQueue>
#include <QMutex>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDateTime>

#include "tokenusageengine.h"

#include <functional>
#include <map>

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

    // ANTS-1253: single tool-provider registry. Each handler receives
    // the JSON-RPC `arguments` object and returns the tool's response
    // as a JSON string (the dispatcher wraps it into a text content
    // block). Absorbs the per-tool setter/member triples that
    // ANTS-1244..1251 each added (12 setters → 1 registrar). The
    // `get_session_info` tool stays inline in the dispatcher — it
    // reads ClaudeIntegration's own state, not an external delegate.
    //
    // ANTS-1419 — registration takes the per-tool CallerCwdContract
    // as a 2nd positional argument so the security contract is a
    // visible part of every registration block, not a separate
    // table lookup. The dispatcher consults the stored contract on
    // the registered entry; the static `callerCwdContractFor`
    // helper is preserved as a back-compat accessor for tests and
    // for tools dispatched inline (get_session_info, tool_info).
    // registerToolProvider asserts the passed contract matches the
    // static table so drift between call-site and table is loud.
    using ToolHandler = std::function<QString(const QJsonObject &args)>;
    enum class CallerCwdContract;
    void registerToolProvider(const QString &name,
                              CallerCwdContract contract,
                              ToolHandler handler);

    // ANTS-1404 — per-tool caller_cwd contract. Recorded once per
    // tool at registration time and consulted by the dispatcher
    // before the provider lambda runs. See docs/specs/ANTS-1404.md.
    enum class CallerCwdContract {
        // Anchorable + leaks if absent — refuse with
        // {ok:false, code:"caller_cwd_required"} when caller_cwd is
        // empty. Group (Phase 3a): get_git_status, last_audit_summary,
        // git_state, verify_changes.
        Required,
        // Anchor when caller_cwd present, focused-tab fallback when
        // absent. Survey-from-outside legitimate. Default for
        // unclassified tools.
        Optional,
        // Per-tab reads that route on `tab` index *or* caller_cwd.
        // Phase 3a classifies but does NOT enforce — the routing-
        // vs-anchoring overlap with ANTS-1392 needs its own spec
        // pass.
        TabSpecific,
        // No per-tab / per-project state; caller_cwd accepted-and-
        // ignored. Phase 3a does not enforce.
        ProcessGlobal,
    };

    // ANTS-1404 — return the classification for `toolName`. Static
    // table inside claudeintegration.cpp; unknown tools default to
    // Optional. See docs/specs/ANTS-1404.md.
    static CallerCwdContract callerCwdContractFor(const QString &toolName);

    // ANTS-1357 — shared with the MainWindow lambdas that emit this
    // exact envelope when m_remoteControl is null (transient startup
    // window). The idempotent-read cache rejects this literal at
    // insert time (INV-5(b)).
    static constexpr const char *kMcpRcUnavailable =
        "{\"ok\":false,\"error\":\"remote-control unavailable\","
        "\"code\":\"no_remote_control\"}";

    // ANTS-1356 — MCP per-tool rate-limit / quota.
    //
    // Three tiers via rateLimitClassFor(toolName):
    //   Cheap        — 60 calls / 60 s (default; most read verbs).
    //   Expensive    — 10 calls / 60 s (audit_run, workspace_search,
    //                  verify_changes, cold_eyes_*, indie_review_*,
    //                  test_audit_*, debt_sweep_scan/apply_fix).
    //   ControlPlane — uncapped (get_session_info, token_usage,
    //                  tool_info, mcp_trace, caller_cwd_info).
    //
    // Sliding-window bucket per (toolName, callerCwd). Monotonic
    // clock in production via s_rateLimitClock; tests pass synthetic
    // nowMs. See docs/specs/ANTS-1356.md.
    // ANTS-1629 — BriefAssembly tier. Higher cap (30/min) for brief-
    // generation verbs that fan out one-call-per-lane during a single
    // /cold-eyes (or sibling) sweep. The default `/cold-eyes` Phase-2
    // step dispatches 12-16 cold_eyes_brief calls in a single parallel
    // batch; the 10/min Expensive cap blocked the recommended pattern.
    // Cost shape: file reads + template assembly (no shell-out, no
    // subagent dispatch) — semantically closer to Cheap than Expensive,
    // but the per-call I/O justifies a tier below Cheap (60/min).
    enum class RateLimitClass {
        Cheap, BriefAssembly, Expensive, ControlPlane };
    struct RateLimitTier { int capPerWindow; qint64 windowMs; };
    static RateLimitClass rateLimitClassFor(const QString &toolName);
    static RateLimitTier  rateLimitTierFor(RateLimitClass cls);

    // Returns 0 if the call is accepted (timestamp appended to bucket
    // as side-effect). Returns retry_after_ms (> 0) if refused.
    // `nowMs` is parameterised so tests can drive synthetic time.
    qint64 rateLimitCheck(const QString &toolName,
                          const QString &callerCwd,
                          qint64 nowMs);

    // ANTS-1771 — canonicalise a caller_cwd into a stable rate-limit
    // bucket key (existing path → canonicalFilePath; non-existent →
    // QDir::cleanPath lexical fallback so synonyms still collapse).
    // Static + pure so the dedup logic is unit-testable.
    static QString canonicaliseCallerKey(const QString &callerCwd);

    // Test-only probes (mirror the existing *ForTest convention).
    int rateLimitBucketCountForTest() const {
        return m_rateLimitBuckets.size();
    }
    int rateLimitBucketDepthForTest(const QString &toolName,
                                    const QString &callerCwd) const {
        const auto it = m_rateLimitBuckets.constFind(
            qMakePair(toolName, callerCwd));
        return (it == m_rateLimitBuckets.cend()) ? 0 : it->tsMs.size();
    }
    // Override per-tier caps for the duration of a test. Reset to
    // restore the 60/10 defaults.
    static void setRateLimitCapsForTest(int cheap, int expensive);
    // ANTS-1629 — additive 3-arg overload covers the BriefAssembly tier.
    static void setRateLimitCapsForTest(
        int cheap, int briefAssembly, int expensive);
    static void resetRateLimitCapsForTest();

    // ANTS-1357 — direct cache API (test-only seam). The integration
    // with the MCP dispatch is exercised in
    // tests/features/mcp_idempotent_read_cache via these methods,
    // bypassing the QLocalSocket round-trip. See docs/specs/ANTS-1357.md.
    QString tryGetIdempotentReadCacheForTest(
        const QString &toolName, const QJsonObject &args) const {
        return tryGetIdempotentReadCache(toolName, args);
    }
    void putIdempotentReadCacheForTest(
        const QString &toolName, const QJsonObject &args,
        const QString &response) {
        maybeInsertIdempotentReadCache(toolName, args, response);
    }
    int idempotentReadCacheSizeForTest() const {
        return m_idempotentReadCache.size();
    }
    QStringList idempotentReadCacheLruForTest() const {
        return QStringList(m_idempotentReadLru.cbegin(),
                           m_idempotentReadLru.cend());
    }

    // ANTS-1284 — in-process MCP dispatch counter. recordCall is
    // invoked from the dispatch site (private member access);
    // reset/buildReport are exposed for RemoteControl::cmdTokenUsage
    // and for tests. Reset additionally fires on MCP `initialize`
    // (Claude Code session start) — wired in onMcpConnection.
    TokenUsageEngine::Snapshot tokenUsageReport(bool includeZero) const {
        return m_tokenUsage.buildReport(includeZero);
    }
    void resetTokenUsage() { m_tokenUsage.reset(); }
    // Test-only convenience: drive recordCall without the full MCP
    // dispatch path. Mirrors processHookEventForTest's pattern.
    // ANTS-1355 — wrapBytes + durationUs defaulted to preserve v1
    // test-callsite compatibility.
    // ANTS-1432 — `success` defaulted to true preserves v2 callsite
    // compatibility; failed-branch tests pass false explicitly.
    void recordTokenUsageForTest(const QString &toolName,
                                 qint64 bytesIn, qint64 bytesOut,
                                 qint64 wrapBytes = 0,
                                 qint64 durationUs = 0,
                                 bool   success    = true) {
        m_tokenUsage.recordCall(toolName, bytesIn, bytesOut,
                                wrapBytes, durationUs, success);
    }

    // ANTS-1294 — frame user-supplied MCP content as data. Wraps a
    // tools/call response payload in <ants_mcp_data tool="..."> so
    // a consuming model can syntactically distinguish "data the
    // server returned" from "instructions from the user". Embedded
    // </ants_mcp_data> close tags in the payload are replaced with
    // a sentinel to neutralise breakout. See docs/specs/ANTS-1294.md.
    static QString wrapMcpData(const QString &toolName,
                               const QString &payload);

    // ANTS-1499 — ETag "304 Not Modified" pattern for read tools.
    // isEtagSupportedTool gates the allowlist (project_layout,
    // roadmap_query, file_outline, last_audit_summary, get_environment,
    // tab_list, subsystem, git_state). etagFor returns the sha256-hex16
    // fingerprint of the raw response bytes (stable across runs).
    // applyEtagPattern injects an `etag` field into a parseable JSON
    // envelope; when the caller passes `etag_match` matching the
    // current etag, returns a short {ok:true,unchanged:true,etag:"…"}
    // envelope instead of the full body. *unchanged is true iff the
    // short envelope path was taken (used by token_usage so cache-hit
    // accounting reflects the smaller payload).
    static bool isEtagSupportedTool(const QString &toolName);
    static QString etagFor(const QString &payload);
    static QString applyEtagPattern(const QString &toolName,
                                    const QJsonObject &args,
                                    const QString &responseText,
                                    bool *unchanged);

    // ANTS-1360 — MCP debug-log tap. The registered `mcp_trace`
    // provider lambda calls this to read a slice of the ring.
    // Filter: id >= since (INV-3 inclusive). Result envelope shape:
    //   {ok, records, next_id, ring_capacity, ring_size}
    // See docs/specs/ANTS-1360.md.
    QJsonObject queryMcpTrace(quint64 since, int limit) const;

    // ANTS-1360 — test-only seam. Drive record/query/clear without
    // standing up the QLocalSocket round-trip. Mirrors ANTS-1357's
    // tryGet/maybeInsert pattern.
    void recordMcpTraceForTest(
        const QString &toolName, const QJsonObject &args,
        qint64 argBytes, qint64 respBytes, qint64 durationUs,
        bool cacheHit, const QString &result) {
        recordMcpTrace(toolName, args, argBytes, respBytes,
                       durationUs, cacheHit, result);
    }
    QJsonObject queryMcpTraceForTest(quint64 since, int limit) const {
        return queryMcpTrace(since, limit);
    }
    int mcpTraceSizeForTest() const {
        return m_mcpTraceRing.size();
    }
    void clearMcpTraceForTest() {
        m_mcpTraceRing.clear();
        m_mcpTraceNextId = 1;
    }

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
    // ANTS-1253: per-tool providers consolidated into a single
    // name-keyed registry. See registerToolProvider() above.
    // ANTS-1419: value type carries the per-tool CallerCwdContract
    // alongside the handler so the dispatcher can read the
    // call-site declaration without a separate table lookup.
    struct RegisteredTool {
        ToolHandler handler;
        CallerCwdContract contract;
    };
    std::map<QString, RegisteredTool> m_toolProviders;

    // ANTS-1284 — per-tool MCP dispatch byte counters. Resets on
    // MCP `initialize` and on explicit token_usage(reset:true).
    TokenUsageEngine::Tracker m_tokenUsage;

    // ANTS-1399 — snapshot of the last `tools/list` array build,
    // populated at the end of the `tools/list` handler. `tool_info`
    // reads from this snapshot to return a single descriptor slice
    // without rebuilding the ~5 KiB array. Lifetime: process; the
    // descriptors are built from compile-time literals so the
    // snapshot never goes stale in practice.
    mutable QJsonArray m_lastToolsList;

    // ANTS-1357 — short-TTL idempotent-read cache.
    // Allowlist: get_cwd / get_environment / tab_list / last_audit_summary.
    // Key: SHA256-hex16(toolName + '\0' + QJsonDocument(args).toJson(Compact)).
    // INVs in docs/specs/ANTS-1357.md.
    struct IdempotentReadEntry {
        qint64  stampMs = 0;
        QString response;
    };
    mutable QHash<QString, IdempotentReadEntry> m_idempotentReadCache;
    mutable QList<QString>                      m_idempotentReadLru;
    static constexpr int    kIdempotentReadCacheCap = 32;
    static constexpr qint64 kIdempotentReadTtlMs    = 100;

    // ANTS-1356 — MCP per-tool rate-limit / quota. Per-(toolName,
    // callerCwd) sliding-window bucket. Pruned on access; empty
    // buckets auto-evict to keep the map size proportional to active
    // load. kRateLimitMapCap bounds map growth via LRU eviction.
    // See docs/specs/ANTS-1356.md.
    struct RateLimitBucket {
        QQueue<qint64> tsMs;  // monotonic ms, strictly ascending
    };
    QHash<QPair<QString, QString>, RateLimitBucket> m_rateLimitBuckets;
    static constexpr int kRateLimitMapCap       = 256;
    static constexpr int kRateLimitCheapCap         = 60;
    static constexpr int kRateLimitBriefAssemblyCap = 30;
    static constexpr int kRateLimitExpensiveCap     = 10;
    static constexpr qint64 kRateLimitWindowMs      = 60'000;

    // ANTS-1351 + ANTS-1397 § 2.4 — inline in-flight gate. Key:
    // (verb, canonicalProjectRoot) → start time (ms epoch). RAII
    // guard removes the slot on return; stale-slot reaper sweeps
    // entries older than aggregate-cap + 30 s on each tryAcquire
    // so a worker-death orphan can't permanently brick the slot.
    mutable QHash<QPair<QString, QString>, qint64> m_verbInFlight;
    mutable QMutex                                 m_verbInFlightMutex;
    static constexpr qint64 kVerbInFlightReapMs = 270'000;  // 240 + 30
public:
    // Public for the dispatch lambdas in mainwindow.cpp. Returns
    // -1 if the slot was free (and reserved on the caller's behalf);
    // otherwise returns the existing start-time so the caller can
    // emit `already_running` + running_since_ms.
    qint64 verbInFlightTryAcquire(const QString &verb,
                                  const QString &projectRoot);
    void   verbInFlightRelease(const QString &verb,
                               const QString &projectRoot);
private:

    // Private helpers — the test-only methods in the public section
    // delegate here, and the dispatch site at processTools uses them
    // directly.
    static bool isIdempotentReadTool(const QString &toolName);
    static QString idempotentReadCacheKey(
        const QString &toolName, const QJsonObject &args);
    QString tryGetIdempotentReadCache(
        const QString &toolName, const QJsonObject &args) const;
    void maybeInsertIdempotentReadCache(
        const QString &toolName, const QJsonObject &args,
        const QString &response);

    // ANTS-1360 — MCP debug-log tap. Ring buffer of last
    // kMcpTraceCap tools/call dispatches. Single-threaded by INV-10
    // (Qt main thread, QLocalSocket readyRead). See
    // docs/specs/ANTS-1360.md.
    struct McpTraceRecord {
        quint64     id          = 0;
        qint64      tsMs        = 0;
        QString     tool;
        QJsonObject argKeys;
        qint64      argBytes    = 0;
        QString     argsSha16;
        qint64      respBytes   = 0;
        qint64      durationUs  = 0;
        bool        cacheHit    = false;
        QString     result;
    };
    QList<McpTraceRecord>  m_mcpTraceRing;
    quint64                m_mcpTraceNextId = 1;
    static constexpr int   kMcpTraceCap     = 200;

    void recordMcpTrace(
        const QString &toolName, const QJsonObject &args,
        qint64 argBytes, qint64 respBytes, qint64 durationUs,
        bool cacheHit, const QString &result);

    // ANTS-1402 — single dispatch-observation hook. Both
    // m_tokenUsage.recordCall and recordMcpTrace see byte-
    // identical input. `result` is "ok" on the success path
    // and the failure code (e.g. "tool_not_found") on the
    // failure path. m_tokenUsage only counts successful calls
    // — failure-branch observer behaviour preserved from the
    // pre-1402 wiring.
    void recordDispatch(
        const QString &toolName, const QJsonObject &argsObj,
        qint64 argBytes, qint64 outBytes, qint64 wrapBytes,
        qint64 durUs, bool cachedHit, const QString &result);
    static QJsonObject argShapeOf(const QJsonObject &args);
    static QString argsSha16Of(const QJsonObject &args);
    static QJsonObject recordToJson(const McpTraceRecord &r);

    // Session tracking
    QString m_activeSessionId;
    QString m_transcriptPath;
    QFileSystemWatcher m_transcriptWatcher;
};
