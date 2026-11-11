#include "claudeintegration.h"
#include "claudeintegration_internal.h"  // ANTS-4919 — promoted helpers
#include "antsversion.h"
#include "guithread.h"
#include "localsockethub.h"  // ANTS-5144 — one listener per path, shared

#include "build_info.h"  // ANTS-1952 — git SHA + build time for serverInfo
#include "debuglog.h"
#include "mcpdeprecation.h"         // ANTS-5485
#include "mcpprojection.h"
#include "mcpspill.h"          // ANTS-2094 — result offload
#include "mcpundeclaredargs.h"  // ANTS-4659 — undeclared-argument survey
#include "secureio.h"
#include "verifytrustprompt.h"  // ANTS-5464 — the method name

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>

#include <exception>
#include <QJsonArray>
#include <QPointer>  // ANTS-2101 — guard MCP socket across nested-loop dispatch
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QCoreApplication>

#include <atomic>
#include <climits>
#include <limits>
#include <memory>
#include <utility>
#include <sys/socket.h>

// ANTS-1356 — monotonic clock used by rateLimitCheck in production.
// Started once in the ClaudeIntegration constructor; tests bypass via
// the synthetic-nowMs parameter so this clock is never read during
// pure-function tests.
static QElapsedTimer s_rateLimitClock;
// ANTS-5090 — the same monotonic clock for the read-cache TTL and the
// in-flight and audit-job reapers, so a suspend or a wall-clock step can
// neither reap a live slot nor serve a stale read. Declared in
// claudeintegration_internal.h.
namespace claudeintegrationdetail {
qint64 monotonicNowMs() { return s_rateLimitClock.elapsed(); }
}  // namespace claudeintegrationdetail
#include <unistd.h>

// ANTS-1415 — the CallerCwdContract::TabSpecific tools whose handler
// honours an explicit `tab` index as an alternate routing key
// (cmdGetText, cmdRecentErrors, cmdLastSelection). The other four
// (get_scrollback, get_last_command, get_environment, get_cwd) ignore
// `tab`, so it must NOT count as a routing key for them — else a stray
// tab:N would bypass the Phase 3b gate and still fall back to the
// focused tab. See docs/specs/ANTS-1415-callercwdcontract-tabspecific.md.
static bool tabSpecificAcceptsTabIndex(const QString &toolName) {
    return toolName == QLatin1String("get_text") ||
           toolName == QLatin1String("recent_errors") ||
           toolName == QLatin1String("last_selection");
}

// ANTS-1659 — the most one hook event or MCP request may buffer: both are
// small, and the cap closes a same-UID OOM vector (deeply nested JSON
// balloons 3-5x in QJson tree allocation). Was 10 MiB.
constexpr qsizetype kMaxSocketRequestBytes = qsizetype{256} * 1024;

ClaudeIntegration::ClaudeIntegration(QObject *parent) : QObject(parent) {
    // ANTS-1356 — start the monotonic clock used by rateLimitCheck.
    // QElapsedTimer::start() is idempotent; calling it on already-
    // started clocks resets — which is fine for the singleton
    // ClaudeIntegration that lives for the whole process lifetime.
    if (!s_rateLimitClock.isValid()) {
        s_rateLimitClock.start();
    }

    // Poll for Claude Code process every 2 seconds. This is only for
    // detecting claude-code starting/stopping under our shell — transcript
    // state changes are event-driven via m_transcriptWatcher below.
    m_pollTimer.setInterval(2000);
    connect(&m_pollTimer, &QTimer::timeout, this, &ClaudeIntegration::pollClaudeProcess);

    // Coalesce bursts from the transcript watcher. During streaming assistant
    // output Claude Code appends many JSONL lines per second; each would
    // otherwise trigger a parse. 50ms is short enough that UI latency stays
    // imperceptible and long enough to collapse a typical write-burst.
    m_transcriptDebounce.setSingleShot(true);
    m_transcriptDebounce.setInterval(50);
    connect(&m_transcriptDebounce, &QTimer::timeout, this, [this]() {
        if (!m_transcriptPath.isEmpty())
            parseTranscriptForState(m_transcriptPath);
    });

    // QFileSystemWatcher wraps inotify; this costs ~1KB of kernel memory per
    // watched path and zero CPU when the file is quiescent. The signal is
    // wired once here instead of per-session to avoid the disconnect/connect
    // dance the old polling code was doing.
    connect(&m_transcriptWatcher, &QFileSystemWatcher::fileChanged,
            this, [this](const QString &) {
        if (!m_transcriptDebounce.isActive())
            m_transcriptDebounce.start();
    });
}

ClaudeIntegration::~ClaudeIntegration() {
    // ANTS-2132 — stop accepting verbs, refuse in-flight GUI marshals, join.
    shutdownDispatchWorker();
    stopHookServer();
    stopMcpServer();
}

// --- Hook Server ---

// ANTS-5236 — in the private runtime directory, never a guessable /tmp name.
// Empty when that directory is unusable, and then no hook server starts.
QString ClaudeIntegration::defaultHookSocketPath() {
    return privateSocketPath(QStringLiteral("claude-hooks-") +
                             QString::number(QCoreApplication::applicationPid()));
}

bool ClaudeIntegration::startHookServer(const QString &socketPath) {
    if (m_hookServer) return true;
    if (socketPath.isEmpty()) return false;

    // ANTS-5144 § 2.2 — every window shares one server per path, so a second
    // window attaches instead of taking the path from the first. The hub
    // keeps ANTS-1132's UserAccessOption and safe-unlink guards.
    m_hookServer = ants::LocalSocketHub::instance().acquire(socketPath);
    if (!m_hookServer) return false;
    m_hookSocketPath = socketPath;
    ants::LocalSocketHub::instance().attach(
        socketPath, this,
        [this] { return !m_windowVisibleProbe || m_windowVisibleProbe(); },
        [this] { onHookConnection(); },
        [this](const QString &sessionId) {
            return m_sessionOwnerProbe && m_sessionOwnerProbe(sessionId);
        },
        [this](const QJsonObject &event) { processHookEvent(event); });
    return true;
}

void ClaudeIntegration::stopHookServer() {
    // ANTS-5144 — detach only: other windows may still serve the server.
    if (m_hookServer) {
        if (auto *hub = ants::LocalSocketHub::existing())
            hub->detach(m_hookSocketPath, this);
        m_hookServer = nullptr;
    }
}


void ClaudeIntegration::onHookConnection() {
    while (m_hookServer->hasPendingConnections()) {
        QLocalSocket *socket = m_hookServer->nextPendingConnection();
        // ANTS-5144 § 2.5 — the server is shared; this owner takes the
        // socket, so destroying the owner closes what it was serving.
        socket->setParent(this);
        // ANTS-1151 — extend the SO_PEERCRED + idle-timeout pattern
        // from RemoteControl::onNewConnection to the Claude hook
        // socket. UserAccessOption + safeToUnlinkLocalSocket
        // already cover the file-side guarantees; the peer check
        // refuses a connection from a process running as a DIFFERENT
        // user. ANTS-5089 — it compares uids only, so a process running
        // as this user (a browser plugin included) is admitted and can
        // send hook events; that is the same-UID trust model
        // (ADR-0004), not a gap this check closes.
        // ANTS-1797 — fail CLOSED: an unavailable socket fd means the peer
        // UID cannot be verified, so the connection must be refused rather
        // than served unauthenticated. (A bare `if (fd >= 0)` guard would
        // skip the check entirely on fd<0.)
        const qintptr fd = socket->socketDescriptor();
        bool peerVerified = false;
        if (fd >= 0) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            if (::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                             SO_PEERCRED, &cred, &len) == 0 &&
                len == sizeof(cred) && cred.uid == ::getuid()) {
                peerVerified = true;
            }
        }
        if (!peerVerified) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        if (!ants::admitLiveConnection(this, socket, m_liveHookConnections)) continue;
        // ANTS-1151 — slow-loris defence. A peer that connects
        // and never sends bytes (or never closes) used to hold a
        // QLocalSocket forever. 5 s idle timeout matches
        // RemoteControl.
        //
        // Indie-review-2026-05-14 lane-3 H1 — LOAD-BEARING: do NOT
        // restart this timer inside readyRead to "extend on activity."
        // The 5 s cap is the only wall-clock bound on a single
        // hook-server RPC; the JSON parse happens once on
        // disconnect against up to 256 KiB of buffered data. A peer
        // that dribbled 1 byte every 4.9 s with a restart-on-read
        // would hold the connection indefinitely and pay one
        // expensive parse at the end. setSingleShot keeps the
        // wall-clock cap absolute regardless of activity.
        QTimer *idleTimer = new QTimer(socket);
        idleTimer->setSingleShot(true);
        idleTimer->setInterval(5000);
        connect(idleTimer, &QTimer::timeout, socket,
                [socket]() { socket->abort(); });
        idleTimer->start();
        // Buffer incoming data — readyRead may fire with partial JSON.
        // ANTS-2216 — the buffer is shared by the two lambdas, not held in a
        // socket property: a property round-trip copied it on every readyRead.
        auto buf = std::make_shared<QByteArray>();
        auto overCap = std::make_shared<bool>(false);
        connect(socket, &QLocalSocket::readyRead, this, [socket, buf, overCap]() {
            if (*overCap) { socket->readAll(); return; }
            *buf += socket->readAll();
            // ANTS-6397 — empty the buffer and latch: the disconnected handler
            // below parses it, so over-cap bytes left in it would be parsed.
            if (buf->size() > kMaxSocketRequestBytes) {
                buf->clear();
                *overCap = true;
                socket->disconnectFromServer();
                return;
            }
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket, buf]() {
            QJsonDocument doc = QJsonDocument::fromJson(*buf);
            // ANTS-5144 § 2.4 — one window processes the event: the one whose
            // tabs track its session, else this connection's target.
            if (doc.isObject())
                ants::LocalSocketHub::instance().deliverHookEvent(
                    m_hookSocketPath, doc.object());
            socket->deleteLater();
        });
    }
}

bool ClaudeIntegration::isFocusedTabSession(const QString &sessionId) const {
    // Tolerate pre-poll state: until pollClaudeProcess has resolved the
    // focused tab's transcript path, accept any session_id rather than
    // silently drop the bootstrap SessionStart that wires m_activeSessionId.
    if (sessionId.isEmpty() || m_transcriptPath.isEmpty()) return true;
    const int slash = m_transcriptPath.lastIndexOf('/');
    const int dot = m_transcriptPath.lastIndexOf('.');
    const int start = slash + 1;
    const int end = (dot > slash) ? dot : m_transcriptPath.size();
    return QStringView(m_transcriptPath).mid(start, end - start) == sessionId;
}

void ClaudeIntegration::processHookEvent(const QJsonObject &event) {
    ++m_hookEventsProcessed;  // ANTS-5144 — on entry, before any gate
    QString hookName = event.value("hook_event_name").toString();
    QString toolName = event.value("tool_name").toString();
    QJsonObject toolInput = event.value("tool_input").toObject();
    // Stash session_id before dispatch so downstream handlers (e.g. the
    // permissionRequested slot in MainWindow) can route the event to
    // the correct per-shell tracker entry by session. The hook server
    // is a single UDS shared across every Claude under any tab, so
    // without this routing the UI would always flag the active tab.
    //
    // Indie-review-2026-05-14 lane-3 M3: read the incoming session_id
    // into a local first; only commit to m_lastHookSessionId AFTER the
    // cold-start drop check. Pre-fix, a dropped sibling-tab event still
    // mutated the singleton's last-seen-session, so a PermissionRequest
    // arriving in the same window could route to the wrong tab.
    const QString incomingSessionId = event.value("session_id").toString();

    // ANTS-1161 — pre-fix, sibling-tab hooks clobbered m_state and
    // emitted stateChanged on the singleton, painting "Claude: bash"
    // on a tab whose Claude was actually in a Thinking turn. Gate
    // every state-mutating branch on the event belonging to the
    // focused tab. PermissionRequest stays ungated because its slot
    // routes via m_lastHookSessionId, not via the singleton state.
    const bool isFocused = isFocusedTabSession(incomingSessionId);

    // ANTS-1860 — log every received hook so tab-dot / prompt-state bugs
    // are diagnosable from the Claude debug category. The success path
    // was previously silent (only the cold-start drop below logged), so
    // ANTS-1858's missing PostToolUse/Stop clear could not be confirmed
    // from the log. Session id truncated to keep the line compact.
    if (DebugLog::enabled(DebugLog::Claude)) {
        ANTS_LOG(DebugLog::Claude,
                 "hook recv: hook=%s tool=%s session=%.8s focused=%s",
                 hookName.isEmpty() ? "-" : hookName.toUtf8().constData(),
                 toolName.isEmpty() ? "-" : toolName.toUtf8().constData(),
                 incomingSessionId.toUtf8().constData(),
                 isFocused ? "yes" : "no");
    }

    // Indie-review 2026-05-13: cold-start tightening. During the 1-3s
    // window between setShellPid()'s synchronous m_transcriptPath
    // clear and the next pollClaudeProcess tick, isFocusedTabSession
    // returns true for ANY incoming event. PreToolUse/PostToolUse/Stop
    // from a sibling tab's still-running Claude would mutate state
    // and emit stateChanged on the singleton. Only SessionStart needs
    // the cold-start fallthrough (it bootstraps m_activeSessionId);
    // every other state-mutating hook should drop silently until poll
    // resolves the transcript path.
    //
    // ANTS-2190 — PermissionRequest is NO LONGER exempt from the cold-start
    // drop. During the window isFocusedTabSession() returns true for ANY
    // session_id (m_transcriptPath is empty), so a sibling tab's
    // PermissionRequest would both commit m_lastHookSessionId and emit
    // permissionRequested, mis-attributing the prompt to the focused tab when
    // two Claude tabs are live. We cannot confirm focus during cold-start, so
    // the safe action is to drop it (mirrors the state-mutating-hook drop): a
    // missed prompt indicator during a 1-3 s window beats a wrong-tab one.
    // Only SessionStart still falls through (it bootstraps m_activeSessionId).
    const bool coldStart = m_transcriptPath.isEmpty();
    const bool isColdStartDroppable = hookName != "SessionStart";
    if (coldStart && isColdStartDroppable) {
        if (DebugLog::enabled(DebugLog::Claude)) {
            ANTS_LOG(DebugLog::Claude,
                     "hook-drop (cold-start): session=%s hook=%s",
                     incomingSessionId.toUtf8().constData(),
                     hookName.toUtf8().constData());
        }
        return;  // Drop without leaking session-id into routing field.
    }

    // The event passed the cold-start gate; commit to last-seen.
    //
    // ANTS-1996 / ANTS-2190 — during cold-start ONLY SessionStart reaches
    // here now (PermissionRequest is dropped above, ANTS-2190). A cold-start
    // SessionStart must NOT commit m_lastHookSessionId: m_transcriptPath is
    // empty so isFocusedTabSession() optimistically returns true for ANY
    // session_id, and a sibling tab's SessionStart would otherwise poison the
    // routing field. Warm-path hooks (incl. PermissionRequest) commit normally.
    if (!(coldStart && hookName == QLatin1String("SessionStart")))
        m_lastHookSessionId = incomingSessionId;

    if (hookName == "SessionStart") {
        if (!isFocused) return;
        m_activeSessionId = event.value("session_id").toString();
        m_state = ClaudeState::Idle;
        emit sessionStarted(m_activeSessionId);
        emit stateChanged(m_state, "session started");
    } else if (hookName == "PreToolUse") {
        if (!isFocused) return;
        m_state = ClaudeState::ToolUse;
        m_currentTool = toolName;
        emit toolStarted(toolName, toolInput.value("command").toString());
        emit stateChanged(m_state, toolName);
    } else if (hookName == "PostToolUse") {
        if (!isFocused) return;
        m_state = ClaudeState::Thinking;
        emit toolFinished(toolName, true);
        emit stateChanged(m_state, "thinking");
        // Hook dialect: already extracted from tool_name/tool_input above.
        updateChangedFiles(toolName, toolInput);
    } else if (hookName == "PostToolUseFailure") {
        if (!isFocused) return;
        emit toolFinished(toolName, false);
    } else if (hookName == "Stop") {
        if (!isFocused) return;
        m_state = ClaudeState::Idle;
        m_currentTool.clear();
        QString reason = event.value("stop_reason").toString();
        emit sessionStopped(reason);
        emit stateChanged(m_state, "idle");
    } else if (hookName == "PermissionRequest") {
        // Ungated — slot consumes m_lastHookSessionId for per-tab routing.
        QString input = toolInput.value("command").toString();
        if (input.isEmpty())
            input = toolInput.value("file_path").toString();
        emit permissionRequested(toolName, input);
    } else if (hookName == "PreCompact") {
        if (!isFocused) return;
        m_state = ClaudeState::Compacting;
        emit stateChanged(m_state, QStringLiteral("compacting"));
    }
}

// --- MCP Server ---

bool ClaudeIntegration::startMcpServer(const QString &socketPath) {
    if (m_mcpServer) return true;
    if (socketPath.isEmpty()) return false;

    // ANTS-5144 § 2.2 — shared per path, as startHookServer.
    m_mcpServer = ants::LocalSocketHub::instance().acquire(socketPath);
    if (!m_mcpServer) return false;
    m_mcpSocketPath = socketPath;
    ants::LocalSocketHub::instance().attach(
        socketPath, this,
        [this] { return !m_windowVisibleProbe || m_windowVisibleProbe(); },
        [this] { onMcpConnection(); });
    return true;
}

void ClaudeIntegration::stopMcpServer() {
    // ANTS-5144 — detach only: other windows may still serve the server.
    if (m_mcpServer) {
        if (auto *hub = ants::LocalSocketHub::existing())
            hub->detach(m_mcpSocketPath, this);
        m_mcpServer = nullptr;
    }
}

// ANTS-1253: single tool-provider registrar. Replaces the 12 per-tool
// setXProvider bodies that ANTS-1244..1251 each added. Each handler
// receives the JSON-RPC `arguments` object and returns the tool's
// response as a JSON string; the dispatcher in handleMcpRequest wraps
// it into a text content block.
// ANTS-1419: a CallerCwdContract value is now part of every
// registration so the per-tool security classification lives next to
// the tool's handler in mcptoolregistry.cpp rather than in a separate
// table. The dispatcher consults the stored contract on the
// registered entry; the static `callerCwdContractFor` table is
// preserved for inline-dispatched tools (get_session_info,
// tool_info) and as a runtime drift-check below — if a future
// maintainer updates the table but not the registration (or vice
// versa) the registration is refused in every build config
// (ANTS-1834), and a debug build additionally aborts via Q_ASSERT_X.
// ANTS-3661 — see the header. The two inline verbs are appended explicitly
// because they never reach m_toolProviders; a registry read alone would leave
// `tool_info` as a permanent unresolved candidate in every doc that names it.
QStringList ClaudeIntegration::registeredToolNames() const {
    QStringList out;
    out.reserve(static_cast<int>(m_toolProviders.size()) + 2);
    for (const auto &entry : m_toolProviders) out << entry.first;
    out << QStringLiteral("get_session_info") << QStringLiteral("tool_info");
    // ANTS-4932 — ants-mcpd answers its forwarded verbs too.
    for (const QString &name : m_forwardedTools)
        if (!out.contains(name)) out << name;
    return out;
}

namespace {
// The refusal a verb's escaped exception becomes (mcp-error-codes.md § 5).
QString handlerFailedEnvelope(const QString &tool, const QString &what) {
    qWarning("MCP verb %s threw: %s", qUtf8Printable(tool), qUtf8Printable(what));
    QJsonObject env;
    env[QStringLiteral("ok")]    = false;
    env[QStringLiteral("code")]  = QStringLiteral("handler_failed");
    env[QStringLiteral("error")] = QStringLiteral("%1: internal error (%2)").arg(tool, what);
    return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
}
}  // namespace

void ClaudeIntegration::registerToolProvider(
    const QString &name,
    CallerCwdContract contract,
    ToolHandler handler) {
    // ANTS-1419 drift assertion — the static table is still
    // queried by tests and by `tools/list` schema massage, so
    // mis-classification between the call-site and the table
    // would leak silently. Compare here and refuse the
    // registration on mismatch.
    const CallerCwdContract tableContract = callerCwdContractFor(name);
    if (tableContract != contract) {
        ANTS_LOG(DebugLog::Claude,
                 "ANTS-1419 contract drift: registerToolProvider(%s) "
                 "passed contract=%d but callerCwdContractFor returned "
                 "%d — update one or the other.",
                 name.toUtf8().constData(),
                 static_cast<int>(contract),
                 static_cast<int>(tableContract));
        Q_ASSERT_X(false, "registerToolProvider",
                   "ANTS-1419: contract drift between call-site and "
                   "callerCwdContractFor table");
        // ANTS-1834 — Q_ASSERT_X compiles out under NDEBUG, so without
        // this a Release build would fall through and register the tool
        // with a possibly-wrong caller_cwd classification (a Required
        // tool silently registered as Optional would bypass the
        // caller_cwd_required refusal at dispatch). Refuse the
        // registration in every build config: the tool goes missing —
        // loud in its own right — rather than running mis-classified.
        return;
    }
    // ANTS-1427 — wrap every handler with a lambda-entry log line so
    // multi-checkpoint debugging sees: lambda-enter (here) → cmd*
    // body checkpoint (per-cmd one-liner) → recordDispatch (final).
    // Zero overhead in production (single bit-test when category off).
    //
    // The same wrapper is the exception boundary. This process hosts every
    // Claude Code session's PTY, so a throw escaping one verb — from a Qt
    // slot or a worker job alike — would terminate it and kill them all.
    ToolHandler wrapped =
        [name, inner = std::move(handler)]
        (const QJsonObject &args) -> QString {
            ANTS_LOG(DebugLog::Claude,
                     "mcp lambda-enter tool=%s arg_keys=%lld",
                     name.toUtf8().constData(),
                     static_cast<long long>(args.size()));
            try {
                return inner(args);
            } catch (const std::exception &e) {
                return handlerFailedEnvelope(name, QString::fromUtf8(e.what()));
            } catch (...) {
                return handlerFailedEnvelope(name, QStringLiteral("unknown exception"));
            }
        };
    m_toolProviders[name] = RegisteredTool{std::move(wrapped), contract, false,
                                           DispatchLane::Shared, {}};
}

// ANTS-2132 — the rc-factory overload. Same registration, plus the one bit
// the dispatcher needs to decide which thread may run this handler.
//
// Eligibility is BOTH halves and neither alone. A handler built by an rc
// factory forwards to a RemoteControl cmd*() and nothing else, so it touches
// no widget of its own -- but a TabSpecific verb's cmd*() reads live terminal
// state through MainWindow (cmdLastSelection resolves a TerminalWidget and
// reads selectedText()), so provenance alone is not enough.
// See docs/specs/ANTS-2132-async-mcp-dispatch.md § 2.4.
void ClaudeIntegration::registerToolProvider(
    const QString &name,
    CallerCwdContract contract,
    RcHandler handler) {
    const bool offThread = handler.offThreadEligible
                        && contract != CallerCwdContract::TabSpecific;
    const DispatchLane lane = handler.lane;
    registerToolProvider(name, contract, std::move(handler.fn));
    // registerToolProvider returns early on a contract-drift refusal without
    // inserting, so only stamp the flag on an entry that actually landed.
    if (auto it = m_toolProviders.find(name); it != m_toolProviders.end()) {
        it->second.offThread = offThread;
        it->second.lane      = lane;   // ANTS-5086 § 2.10
    }
}

// ANTS-2132 § 2.8 — the reply-later overload. Same registration and the same
// contract-drift refusal. The entry's `handler` is left empty: the dispatcher
// checks `deferred` first and never calls it.
void ClaudeIntegration::registerToolProvider(
    const QString &name,
    CallerCwdContract contract,
    DeferredToolHandler handler) {
    registerToolProvider(name, contract, ToolHandler{});
    if (auto it = m_toolProviders.find(name); it != m_toolProviders.end())
        it->second.deferred =
            [name, inner = std::move(handler)]
            (const QJsonObject &args, const std::function<void(QString)> &reply) {
                // A throw before `reply` runs answers the call once; the
                // dispatcher drops any second reply.
                try {
                    inner(args, reply);
                } catch (const std::exception &e) {
                    reply(handlerFailedEnvelope(name, QString::fromUtf8(e.what())));
                } catch (...) {
                    reply(handlerFailedEnvelope(name, QStringLiteral("unknown exception")));
                }
            };
}

// ANTS-1360 — MCP debug-log tap. Top-level shape only — no recursion
// into nested objects/arrays (INV-6). Returns a JSON object whose
// keys mirror `args`' keys and whose values are short type tags.
QJsonObject ClaudeIntegration::argShapeOf(const QJsonObject &args) {
    QJsonObject shape;
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        const QJsonValue &v = it.value();
        QString tag;
        switch (v.type()) {
        case QJsonValue::Null:   tag = QStringLiteral("null");   break;
        case QJsonValue::Bool:   tag = QStringLiteral("bool");   break;
        case QJsonValue::String: tag = QStringLiteral("string"); break;
        case QJsonValue::Double: {
            // Best-effort int/double distinction: Qt stores all
            // numerics as double, but a "looks like an int" check
            // helps debug-readability. Spec § 2.4.
            const double d = v.toDouble();
            const qint64 i = static_cast<qint64>(v.toVariant().toLongLong());
            tag = (d == static_cast<double>(i))
                ? QStringLiteral("int")
                : QStringLiteral("double");
            break;
        }
        case QJsonValue::Array:
            tag = QStringLiteral("array<%1>").arg(v.toArray().size());
            break;
        case QJsonValue::Object:
            tag = QStringLiteral("object<%1>").arg(v.toObject().size());
            break;
        default:
            tag = QStringLiteral("unknown");
            break;
        }
        shape.insert(it.key(), tag);
    }
    return shape;
}

QString ClaudeIntegration::argsSha16Of(const QJsonObject &args) {
    const QByteArray compact =
        QJsonDocument(args).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(
        QCryptographicHash::hash(compact, QCryptographicHash::Sha256)
            .toHex().left(16));
}

QJsonObject ClaudeIntegration::recordToJson(const McpTraceRecord &r) {
    QJsonObject o;
    o["id"]          = static_cast<qint64>(r.id);
    o["ts_ms"]       = r.tsMs;
    o["tool"]        = r.tool;
    o["arg_keys"]    = r.argKeys;
    o["arg_bytes"]   = r.argBytes;
    o["raw_bytes"]   = r.rawBytes;   // ANTS-2135 — raw inbound frame size
    o["args_sha16"]  = r.argsSha16;
    o["resp_bytes"]  = r.respBytes;
    o["duration_us"] = r.durationUs;
    o["cache_hit"]   = r.cacheHit;
    o["result"]      = r.result;
    return o;
}

void ClaudeIntegration::recordMcpTrace(
    const QString &toolName, const QJsonObject &args,
    qint64 argBytes, qint64 rawBytes, qint64 respBytes,
    qint64 durationUs, bool cacheHit, const QString &result) {
    // INV-5: mcp_trace never records itself.
    if (toolName == QLatin1String("mcp_trace")) return;
    McpTraceRecord rec;
    rec.id         = m_mcpTraceNextId++;
    rec.tsMs       = QDateTime::currentMSecsSinceEpoch();
    rec.tool       = toolName;
    rec.argKeys    = argShapeOf(args);
    rec.argBytes   = argBytes;
    rec.rawBytes   = rawBytes;   // ANTS-2135
    rec.argsSha16  = argsSha16Of(args);
    rec.respBytes  = respBytes;
    rec.durationUs = durationUs;
    rec.cacheHit   = cacheHit;
    rec.result     = result;
    m_mcpTraceRing.append(rec);
    if (m_mcpTraceRing.size() > kMcpTraceCap) {
        m_mcpTraceRing.removeFirst();
    }
}

// ANTS-4457 — which dispatch results count as successful byte spend.
// A whitelist, so a result added later defaults to failure rather than
// silently counting as success.
bool ClaudeIntegration::dispatchResultIsSuccess(const QString &result) {
    // "etag_unchanged" is the CHEAPEST successful outcome the server
    // has: the caller got exactly what it asked for and the payload was
    // skipped. Deriving success as equality with "ok" booked every 304
    // into the failed accumulators — the counters ANTS-1432 added to
    // measure waste on failure, which then counted the largest saving
    // as waste.
    return result == QLatin1String("ok")
        || result == QLatin1String("etag_unchanged");
}

// ANTS-4457 — the refusal code a handler's OWN envelope carries, or
// empty when the body is not a refusal. dispatchResult is set only by
// dispatcher-level conditions, so a handler that ran and returned
// ok:false (bad_args, bad_section, no_roadmap_loaded, …) was recorded
// as "ok" by both token_usage and mcp_trace. There is no central
// refusal-envelope helper, so the dispatch site is the only place this
// can be observed.
QString ClaudeIntegration::handlerRefusalCode(const QString &responseText) {
    // ANTS-5090 — this answer also sets MCP's isError, so it must hold at
    // any size. A successful payload can be megabytes, and a second full
    // JSON parse of every one would sit on the dispatch path, so a large
    // body is parsed only when its text could be a refusal. A match that
    // is an item's field, not the envelope's, is settled by the parse.
    constexpr int kParseWithoutScanChars = 8 * 1024;
    static const QRegularExpression kOkFalse(QStringLiteral(R"("ok"\s*:\s*false)"));
    if (responseText.size() > kParseWithoutScanChars &&
        !kOkFalse.match(responseText).hasMatch())
        return {};
    QJsonParseError perr{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseText.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return {};
    const QJsonObject obj = doc.object();
    // Default true: many payloads carry no `ok` field at all, and an
    // absent field is not a refusal.
    if (obj.value(QStringLiteral("ok")).toBool(true)) return {};
    const QString code = obj.value(QStringLiteral("code")).toString();
    return code.isEmpty() ? QStringLiteral("handler_refused") : code;
}

// ANTS-1402-INV-2 — single dispatch-observation hook. Tees the
// same numbers to m_tokenUsage and recordMcpTrace.
// ANTS-1432 — recordCall now fires on every dispatch with a
// `success` flag derived from `result`. The pre-1432 short-circuit
// (recordCall skipped when result != "ok") meant per-tool byte
// counters under-reported failed-call cost.
void ClaudeIntegration::recordDispatch(
    const QString &toolName, const QJsonObject &argsObj,
    qint64 argBytes, qint64 rawBytes, qint64 outBytes, qint64 wrapBytes,
    qint64 durUs, bool cachedHit, const QString &result) {
    // ANTS-1432 — recordCall now fires on every dispatch. The engine
    // routes the byte counts into success- or failed-* accumulators
    // based on `success`. Pre-1432 behaviour skipped failed branches
    // entirely; that masked waste-on-failure cost (Vestige CC's
    // 2026-05-16 observation: "MCP cost tokens for the failed query
    // and saved none").
    const bool succeeded = dispatchResultIsSuccess(result);
    // ANTS-5104 — a call naming no registered tool is counted under one
    // entry, so arbitrary names cannot grow the counter map past the
    // registered tools (ANTS-1284 INV-8). Its failed-call bytes still show.
    const QString trackedTool = result == QLatin1String("tool_not_found")
        ? QStringLiteral("(unknown tool)") : toolName;
    m_tokenUsage.recordCall(trackedTool, argBytes, outBytes,
                            wrapBytes, durUs, succeeded);
    recordMcpTrace(toolName, argsObj, argBytes, rawBytes, outBytes,
                   durUs, cachedHit, result);
    // ANTS-1427 — per-dispatch audit trail. Gated on
    // DebugLog::Claude so production is a single bit-test.
    // recordDispatch is the unique observation point (ANTS-1402);
    // logging here means one line per MCP call, no double-count.
    ANTS_LOG(DebugLog::Claude,
             "mcp dispatch tool=%s result=%s "
             "arg_b=%lld out_b=%lld wrap_b=%lld dur_us=%lld cached=%s",
             toolName.toUtf8().constData(),
             result.toUtf8().constData(),
             static_cast<long long>(argBytes),
             static_cast<long long>(outBytes),
             static_cast<long long>(wrapBytes),
             static_cast<long long>(durUs),
             cachedHit ? "yes" : "no");
    // ANTS-3579 — attribute this call's saved BYTES to its project. Grouped with
    // the tokens-saved emit below (both are the pill's concern). The key is the
    // canonicalised caller_cwd (no MainWindow — QFileInfo::canonicalFilePath is
    // static), memoised in a bounded map (§ 6). Empty/nonexistent dir → drop
    // (still counted globally). Success-only; per-call floor at 0 (INV-1).
    if (succeeded) {
        const QString cwd = argsObj.value(QStringLiteral("caller_cwd")).toString();
        if (!cwd.isEmpty()) {
            QString root = m_callerCwdRootMemo.value(cwd);
            if (root.isEmpty()) {
                root = QFileInfo(cwd).canonicalFilePath();
                if (!root.isEmpty()) {
                    if (m_callerCwdRootMemo.size() >= kMaxTokenProjects)
                        m_callerCwdRootMemo.clear();   // bounded memo (§ 6)
                    m_callerCwdRootMemo.insert(cwd, root);
                }
            }
            if (!root.isEmpty()) {
                const qint64 rawSaved =
                    TokenUsageEngine::Tracker::baselineFor(toolName)
                        - (argBytes + outBytes);
                const qint64 saved = rawSaved > 0 ? rawSaved : 0;
                // Cap the live map: evict the least-recently-touched root before
                // admitting a new one (INV-5b, "oldest wins").
                if (!m_sessionSavedBytesByProject.contains(root) &&
                    m_sessionSavedBytesByProject.size() >= kMaxTokenProjects) {
                    QString victim;
                    quint64 oldest = 0;
                    bool have = false;
                    for (auto it = m_projectTouchSeq.constBegin();
                         it != m_projectTouchSeq.constEnd(); ++it)
                        if (!have || it.value() < oldest) {
                            oldest = it.value(); victim = it.key(); have = true;
                        }
                    if (!victim.isEmpty()) {
                        m_sessionSavedBytesByProject.remove(victim);
                        m_projectTouchSeq.remove(victim);
                    }
                }
                m_sessionSavedBytesByProject[root] += saved;
                m_projectTouchSeq[root] = ++m_projectTouchCounter;
            }
        }
    }
    // ANTS-3572 — drive the tokens-saved chip from the single dispatch hook.
    // ANTS-5104 — the total alone; building and sorting the per-tool report
    // on every dispatch cost more than the one number the chip shows.
    emit tokensSavedUpdated(m_tokenUsage.totalSaved());
}

void ClaudeIntegration::endTokenSession() {
    // ANTS-3572 — sole reset path (see docs/specs/ANTS-3572-mcp-tokens-saved-status-bar.md). Emit BEFORE
    // the reset so MainWindow's same-thread (DirectConnection) fold slot
    // snapshots the intact session total; then clear; then blank the chip.
    emit tokenSessionEnding();
    m_tokenUsage.reset();
    // ANTS-3579 (INV-12) — clear the per-project live maps AFTER the fold above
    // has snapshotted them (the fold runs synchronously inside the emit); never
    // before, or the fold would see an empty map (silent per-project loss).
    m_sessionSavedBytesByProject.clear();
    m_projectTouchSeq.clear();
    m_callerCwdRootMemo.clear();
    m_projectTouchCounter = 0;
    emit tokensSavedUpdated(0);
}

QJsonObject ClaudeIntegration::queryMcpTrace(
    quint64 since, int limit) const {
    // INV-4: limit clamps to [1, ring_capacity].
    if (limit < 1)             limit = 1;
    if (limit > kMcpTraceCap)  limit = kMcpTraceCap;
    QJsonArray records;
    quint64 maxReturnedId = 0;
    int emitted = 0;
    for (const McpTraceRecord &r : m_mcpTraceRing) {
        if (r.id < since) continue;              // INV-3: id >= since
        if (emitted >= limit) break;             // INV-4 cap
        records.append(recordToJson(r));
        if (r.id > maxReturnedId) maxReturnedId = r.id;
        ++emitted;
    }
    QJsonObject out;
    out["ok"]            = true;
    out["records"]       = records;
    // INV-3 next_id: max(returned)+1 on non-empty; since echoed on empty.
    out["next_id"]       = records.isEmpty()
        ? static_cast<qint64>(since)
        : static_cast<qint64>(maxReturnedId + 1);
    out["ring_capacity"] = kMcpTraceCap;
    out["ring_size"]     = m_mcpTraceRing.size();
    return out;
}

namespace {
// ANTS-5072 — INV-18's test seam: the thread transformReply last ran on.
std::atomic<QThread *> s_lastReplyTransformThread{nullptr};
}  // namespace

QThread *ClaudeIntegration::lastReplyTransformThreadForTest() {
    return s_lastReplyTransformThread.load();
}

void ClaudeIntegration::resetReplyTransformThreadForTest() {
    s_lastReplyTransformThread.store(nullptr);
}

// ANTS-2132 — hand a verb to the dispatch worker and return to the event
// loop. The GUI thread neither runs the verb nor waits for it, which is the
// whole point: it stops being blocked for the verb's duration.
//
// Returns false when the queue is full. The caller must then refuse, because
// no reply is coming from this path.
bool ClaudeIntegration::postToolDispatch(const McpCallContext &ctx,
                                         const ToolHandler &handler,
                                         DispatchLane lane) {
    return postWorkerJob([this, ctx, handler]() {
        // On the worker. The handler forwards to a RemoteControl cmd*(); any
        // MainWindow read inside it goes through ants::onGuiThread.
        // ANTS-5072 — the reply transforms run here too, so a large body is
        // not re-shaped on the GUI thread (spec § 2.9).
        const ReplyTransform reply = transformReply(ctx, handler(ctx.args));
        // Back to the GUI thread to finish the reply. The socket is only ever
        // written from there.
        QMetaObject::invokeMethod(this, [this, ctx, reply]() {
            finishToolDispatch(ctx, reply);
        }, Qt::QueuedConnection);
    }, lane);
}

// ANTS-2132 § 2.7 — the one entry onto the dispatch worker, shared by MCP
// verbs and the remote-control socket's worker routes, so both serialise on
// one thread and count against one cap.
bool ClaudeIntegration::postWorkerJob(std::function<void()> job,
                                      DispatchLane lane) {
    if (m_dispatchShuttingDown) return false;
    // The cap counts the executing job as well as the queued ones, so the
    // 65th outstanding call is the first refused (spec § 2.6, INV-10). Both
    // lanes count against it (§ 2.10).
    if (m_dispatchInFlight.loadAcquire() >= kMaxDispatchQueue) return false;

    const bool bulk = (lane == DispatchLane::Bulk);
    QThread *&worker = bulk ? m_bulkWorker : m_dispatchWorker;
    QObject *&sink   = bulk ? m_bulkSink : m_dispatchSink;
    if (!worker) {
        worker = new QThread(this);
        worker->setObjectName(bulk ? QStringLiteral("ants-mcp-bulk")
                                   : QStringLiteral("ants-mcp-dispatch"));
        // No parent: a QObject cannot be parented across threads. Deleted in
        // shutdownDispatchWorker() once the thread has been joined.
        sink = new QObject();
        sink->moveToThread(worker);
        worker->start();
    }

    m_dispatchInFlight.fetchAndAddOrdered(1);
    QMetaObject::invokeMethod(sink, [this, job = std::move(job)]() {
        job();
        m_dispatchInFlight.fetchAndSubOrdered(1);
    }, Qt::QueuedConnection);
    return true;
}

// ANTS-2132 — teardown, in the one order that cannot deadlock.
//
// Refuse this worker's GUI marshals BEFORE joining, so none is posted after
// this point. A worker already parked in a BlockingQueuedConnection is waiting
// for this thread, so a bare wait() would deadlock (ANTS-5113);
// joinRefusingMarshals delivers that marshal without running it, which releases
// the worker. This join is INV-7's sole exception: the GUI thread never blocks
// on the worker while SERVING a request.
//
// Only this instance's worker is refused. File → New Window builds a second
// MainWindow with its own ClaudeIntegration, and the first window's marshals
// must still be served after the second closes (ANTS-5142, spec INV-17).
void ClaudeIntegration::shutdownDispatchWorker() {
    m_dispatchShuttingDown = true;
    // ANTS-5086 — BOTH lanes are refused before EITHER is joined (spec § 2.6):
    // joinRefusingMarshals serves a marshal from any thread not refused, so a
    // bulk-lane marshal delivered during the shared join would otherwise run.
    if (m_dispatchWorker) ants::setGuiMarshalRefused(m_dispatchWorker, true);
    if (m_bulkWorker) ants::setGuiMarshalRefused(m_bulkWorker, true);
    if (m_dispatchWorker) m_dispatchWorker->quit();
    if (m_bulkWorker) m_bulkWorker->quit();
    // Joined, so drop each entry: a later thread reusing the address must not
    // inherit the refusal.
    if (m_dispatchWorker) {
        ants::joinRefusingMarshals(m_dispatchWorker);
        ants::setGuiMarshalRefused(m_dispatchWorker, false);
    }
    if (m_bulkWorker) {
        ants::joinRefusingMarshals(m_bulkWorker);
        ants::setGuiMarshalRefused(m_bulkWorker, false);
    }
    delete m_dispatchSink;
    m_dispatchSink = nullptr;
    delete m_bulkSink;
    m_bulkSink = nullptr;
    // ~QObject removes events still posted to `this`, so a result that
    // finished after the join never reaches a half-destroyed object.
}

// ANTS-5072 — the unknown-arg advisory's key set. It reads m_toolParamKeys,
// which handleMcpRequest writes, so the dispatcher computes it on the GUI
// thread and transformReply only applies it (spec § 2.9).
QStringList ClaudeIntegration::ignoredArgKeysFor(
    const QString &toolName, const QJsonObject &argsObj) const {
    QStringList ignoredArgKeys;
    if (m_toolParamKeys.contains(toolName))
        ignoredArgKeys = mcp::ignoredArgs(
            argsObj, m_toolParamKeys.value(toolName),
            honouredDispatchArgs(toolName));
    return ignoredArgKeys;
}

// ANTS-4578 — which dispatch-layer args THIS verb actually honours. The
// predicates live here, so the pure helper cannot ask them; previously it
// exempted all four unconditionally and a `fields` sent to a verb with no
// projection support did nothing and said nothing.
QSet<QString> ClaudeIntegration::honouredDispatchArgs(const QString &toolName) {
    QSet<QString> honoured;
    if (isEtagSupportedTool(toolName))
        honoured.insert(QStringLiteral("etag_match"));
    // ANTS-4524 — `fields` is not here: it is universal now
    // (mcp::isUniversalDispatchArg), so it can never be the
    // dropped argument this advisory exists to name.
    if (mcp::isCompactArgTool(toolName))
        honoured.insert(QStringLiteral("compact"));
    if (mcp::isOffloadEligible(toolName))
        honoured.insert(QStringLiteral("offload"));
    return honoured;
}

QHash<QString, QSet<QString>> ClaudeIntegration::verbArgsSnapshot() const {
    QMutexLocker lock(&m_verbArgsMutex);
    return m_verbArgsPublished;
}

// ANTS-2132 / ANTS-5072 — the MCP reply transforms, lifted out of
// onMcpConnection()'s readyRead lambda. The body below is the inline code in
// the same order. It reads no member, so an off-thread verb runs it on the
// dispatch worker; finishToolDispatch keeps only what touches this object
// (spec § 2.9). No path keeps its own copy.
//
// The locals the lambda held are re-bound as aliases rather than rewritten to
// ctx.* on purpose: a rename would change the argument spellings that many
// source-scrape tests search for, and a scrape that silently stops matching is
// a guard that stops guarding. For the same reason this comment must not quote
// one of those literals -- doing so plants a false first match ahead of the
// real call site and breaks the ordering assertions.
ClaudeIntegration::ReplyTransform ClaudeIntegration::transformReply(
    const McpCallContext &ctx, QString responseText) {
    s_lastReplyTransformThread.store(QThread::currentThread());
    ReplyTransform reply;
    const QString     &toolName    = ctx.toolName;
    const QJsonObject &argsObj     = ctx.args;
    const bool         cachedHit   = ctx.cachedHit;
    const bool         cacheable   = ctx.cacheable;
    const bool         toolHandled = ctx.toolHandled;
    const QStringList &ignoredArgKeys = ctx.ignoredArgKeys;
    // ANTS-2175 — unknown-arg advisory. Diff the call's arg keys
    // against the verb's declared inputSchema properties (plus the
    // universal dispatch-layer args, handled inside mcp::ignoredArgs)
    // and, when a key is unrecognised, attach a non-fatal
    // `ignored_args:[...]` field to the success envelope so a
    // typo'd / stale param surfaces on the first call rather than
    // masquerading as a working filter (the trigger was `query=`
    // passed to roadmap_query, silently dropped). Runs only on a
    // freshly-dispatched call (skip cache hits — the cached body
    // already carries the advisory for those args) and BEFORE the
    // cache insert so a future hit returns it too. Parses the body
    // only when there IS an unrecognised arg (rare), so the
    // steady-state cost is one cheap key-set diff. Empty
    // m_toolParamKeys (no tools/list yet) degrades to no advisory.
    //
    // ANTS-4525 — REFUSALS are annotated too. They used to be left
    // untouched on the reasoning that the caller already reads the
    // error, and that is backwards for the case it matters most
    // in: a caller with a wrong mental model of a verb passes wrong
    // ARGUMENTS and gets a refusal, so the advisory that would
    // correct the model is suppressed precisely because it
    // refused. mcp::withIgnoredArgs only ADDS a key, so the
    // ANTS-2112 refusal floor is untouched by construction.
    //
    // ANTS-4626 — carried as a key list because it is applied on
    // BOTH sides of the offload below. It is computed even on a
    // cache hit, where the pre-offload apply is skipped (the
    // cached body already carries it) but the post-offload one is
    // still owed, since the offload discards it either way.
    if (toolHandled && !cachedHit && !ignoredArgKeys.isEmpty()) {
        responseText =
            mcp::withIgnoredArgs(responseText, ignoredArgKeys);
    }
    // ANTS-1357 — the body the cache stores on miss-success. The
    // insert itself is finishToolDispatch's, on the GUI thread
    // (ANTS-5072). The cache stores the un-etagged response so
    // subsequent callers can either reuse it verbatim OR
    // short-circuit via etag_match against the same fingerprint.
    if (toolHandled && cacheable && !cachedHit) {
        reply.cacheBody = responseText;
    }
    // ANTS-1499 — ETag short-circuit. Runs after the
    // idempotent-read cache so the cached body stays in its
    // canonical (un-etagged) form, and before the wrap so
    // the etag is computed against the actual JSON envelope
    // (not the <ants_mcp_data> wrapper bytes which would
    // shift the hash for every tool name change).
    bool etagUnchanged = false;
    if (toolHandled && isEtagSupportedTool(toolName)) {
        responseText = applyEtagPattern(
            toolName, argsObj, responseText, &etagUnchanged);
    }
    // ANTS-1720 — `fields=` projection. Runs after the etag
    // short-circuit (so the etag is computed on the
    // unfiltered canonical body) and before the wrap (so the
    // hash covers the JSON envelope, not the wrapper). Skipped
    // on the etag short-circuit — {ok,unchanged,etag} has no
    // content to narrow. To retain the etag through a narrowed
    // call, the caller lists "etag" in `fields`.
    // ANTS-4524 — EVERY verb, no allowlist. It was gated on one,
    // so a caller passing `fields=` to any other verb had it
    // dropped, and the per-verb workaround was paid one verb at a
    // time (ANTS-4523, ANTS-4663, ANTS-4665). projectFields is a
    // pure top-level key filter with a refusal floor and a 304
    // floor, which is what makes it safe on an envelope this
    // dispatcher has never seen.
    // ANTS-5356 — it runs after the offload below for a call naming a key only
    // the offload writes.
    const bool fieldsAfterOffload = mcp::fieldsWaitForOffload(toolName, argsObj);
    if (toolHandled && !etagUnchanged) {
        const QJsonValue fv =
            argsObj.value(QStringLiteral("fields"));
        if (fv.isArray() && !fieldsAfterOffload) {
            responseText = mcp::projectFields(
                responseText, fv.toArray());
        }
    }
    // ANTS-2091 — opt-in `compact:true` drops dead-weight
    // fields (null / false / "" / [] / {}) the model never
    // reads. Runs after fields=, so it compacts the narrowed
    // body; skipped on a 304, which is already minimal. Protected
    // keys (ok/code/error/etag/found/unchanged) survive at
    // every level (see mcp::compactEnvelope).
    // ANTS-2085 — resolve `compact` from the per-call arg when
    // present (true OR false both win), else fall back to the
    // session/user terse default (mcp::terseDefault(), driven by
    // the claude.mcp_terse_responses config key — on by default so
    // token-saving needs no per-call flag). Absent ⟺ default makes
    // the fallback lossless; a caller needing empty-vs-absent
    // passes compact:false.
    // ANTS-4524 — compaction did NOT follow `fields=` into being
    // universal, and the reason is the default: a transform nobody
    // asked for is what shipped ANTS-4673. isCompactArgTool gates
    // the verbs declaring the argument at all; isDefaultCompactTool
    // is the separate answer for an ABSENT one.
    if (toolHandled && !etagUnchanged &&
        mcp::isCompactArgTool(toolName)) {
        const QJsonValue compactArg =
            argsObj.value(QStringLiteral("compact"));
        const bool wantCompact = compactArg.isBool()
            ? compactArg.toBool()
            : (mcp::isDefaultCompactTool(toolName)
               && !mcp::isDefaultCompactExemptCall(toolName, argsObj)
               && mcp::terseDefault());
        if (wantCompact)
            responseText = mcp::compactEnvelope(responseText);
    }
    // ANTS-2081 / ANTS-2086 — append etag-reuse + leaner-mode
    // nudges to large read responses. After the etag/fields
    // steps so the hint never perturbs the etag hash or a
    // narrowed body (see maybeAppendReadHints gating).
    if (toolHandled) {
        responseText = mcp::appendReadHints(
            toolName, argsObj, responseText, etagUnchanged);
    }

    // ANTS-2090 — opt-in columnar encoding for homogeneous-array
    // read replies. After appendReadHints (which operates on
    // normal JSON top-level scalars, untouched by tabularize) and
    // before offloadBody — a smaller tabular body may now fit
    // under the spill threshold, and if it still spills the spill
    // file holds valid tabular JSON (INV-8/INV-9). Per-call only
    // (encoding:"tabular"); never a session default, because it
    // changes a field's SHAPE and the caller must know how to
    // decode {__cols__,__rows__}. tabularize is self-guarding per
    // array (no tool-name predicate) — it no-ops on refusals,
    // 304s, and arrays that don't shrink. See docs/specs/ANTS-2090-tabular-columnar-encoding-homogeneous.md.
    if (toolHandled && !etagUnchanged &&
        argsObj.value(QStringLiteral("encoding")).toString()
            == QStringLiteral("tabular")) {
        responseText = mcp::tabularize(responseText);
    }

    // ANTS-2218 — raw (verbatim) framing for content reads. Read
    // here, before the offload below, so it can suppress it: an
    // agent that asked for true bytes must not be handed a
    // head+pointer envelope instead. Honoured only for the
    // isRawEligible read verbs (read_region/read_regions/
    // workspace_search); see the wrap branch below.
    const bool rawRequested =
        mcp::isRawEligible(toolName) &&
        argsObj.value(QStringLiteral("raw")).toBool();

    // ANTS-2094 — proactive result offload (observation masking).
    // After every token-trim transform (fields/compact/hints) and
    // before the wrap: spill an over-threshold body to a
    // content-addressed cache file and replace it with a small
    // head+pointer envelope. Opt-in (per-call offload:true or the
    // session default). The head guard (bodyBytes > head size)
    // keeps offload a net saving; fail-open returns the body
    // unchanged on any write failure. See docs/specs/ANTS-2094-proactive-server-side-result-offload.md.
    if (toolHandled && !etagUnchanged && !rawRequested &&
        mcp::isOffloadEligible(toolName) &&
        mcp::offloadRequested(argsObj)) {
        // ANTS-3552 — the >=threshold / >head boundary is the
        // extracted mcp::shouldOffload predicate (behaviourally
        // tested; offloadBody has no internal threshold guard).
        const qint64 bodyBytes = responseText.toUtf8().size();
        // ANTS-5364 — the threshold is per call: a sized roadmap_query
        // raises it (mcp::offloadThresholdFor).
        if (mcp::shouldOffload(
                bodyBytes, mcp::offloadThresholdFor(toolName, argsObj))) {
            responseText = mcp::offloadBody(toolName, responseText, argsObj);
            // ANTS-4626 — offloadBody builds a FRESH head+pointer
            // envelope from its own keys, so the ANTS-2175
            // advisory attached above is discarded with the rest
            // of the body. That put it exactly backwards: the
            // offload fires on the largest answers, and a filter
            // that was silently ignored is what MAKES an answer
            // too large. The reply is then a superset of what was
            // asked for — the one shape a caller cannot detect,
            // since a filter that matched nothing and a filter
            // that never ran both return rows, and the second
            // returns more.
            //
            // Re-applied here rather than preserved inside
            // offloadBody, which is deliberately content-agnostic
            // (it Q_UNUSEDs the tool name) and whose parse is
            // RAM-capped, so a body over that cap could not carry
            // the advisory across at all. withIgnoredArgs only
            // ADDS a key, so the pointer envelope is untouched.
            if (!ignoredArgKeys.isEmpty()) {
                responseText = mcp::withIgnoredArgs(
                    responseText, ignoredArgKeys);
            }
        }
    }
    if (toolHandled && !etagUnchanged && fieldsAfterOffload)
        responseText = mcp::projectFields(
            responseText, argsObj.value(QStringLiteral("fields")).toArray());

    if (toolHandled) {
        // ANTS-1294 — frame user-supplied content as data,
        // not instructions. Control-plane tools (server-
        // generated state) skip the wrap so a caller can
        // syntactically distinguish structural metadata
        // from content. See docs/specs/ANTS-1294-mcp-output-sanitisation-frame-user.md.
        const bool isControlPlane =
            (toolName == QStringLiteral("get_session_info") ||
             toolName == QStringLiteral("token_usage") ||
             // ANTS-1399 — tool_info returns a descriptor
             // slice (server-generated metadata about other
             // tools), not user content. Bypass the wrap
             // for the same reason get_session_info does.
             toolName == QStringLiteral("tool_info"));
        // ANTS-2218 — raw verbatim frame (opt-in, read verbs) vs
        // the default lossy tag-scrub. Control-plane bypasses both.
        const QString wrapped =
            isControlPlane ? responseText
            : rawRequested ? wrapMcpDataRaw(toolName, responseText)
                           : wrapMcpData(toolName, responseText);
        reply.wrapped       = wrapped;
        reply.etagUnchanged = etagUnchanged;
        // ANTS-4457 — the handler's own refusal, read here while the
        // transformed body is in hand; finishToolDispatch decides whether
        // it overrides the dispatcher's result.
        reply.refusalCode = handlerRefusalCode(responseText);
        // ANTS-1284 byte-count contract: arg/out bytes measure the
        // wrapped payload (what actually crosses the wire).
        reply.argBytes  = QJsonDocument(argsObj)
            .toJson(QJsonDocument::Compact).size();
        reply.outBytes  = wrapped.toUtf8().size();
        const qint64 rawBytes = responseText.toUtf8().size();
        reply.wrapBytes = reply.outBytes - rawBytes;        // ANTS-1355 INV-3
    }
    return reply;
}

QJsonObject ClaudeIntegration::toolCallResult(const QString &wrapped,
                                              const QString &refusalCode) {
    QJsonObject block;
    block["type"] = "text";
    block["text"] = wrapped;
    QJsonObject result;
    result["content"] = QJsonArray{block};
    if (!refusalCode.isEmpty()) result["isError"] = true;
    return result;
}

// ANTS-2132 / ANTS-5072 — the GUI-thread half of the reply: the cache insert,
// recordDispatch and the socket write. The transforms already ran in
// transformReply, wherever the handler ran.
void ClaudeIntegration::finishToolDispatch(McpCallContext ctx,
                                           ReplyTransform reply) {
    const QString     &toolName    = ctx.toolName;
    const QJsonObject &argsObj     = ctx.args;
    const bool         cachedHit   = ctx.cachedHit;
    const bool         cacheable   = ctx.cacheable;
    const bool         toolHandled = ctx.toolHandled;
    const QElapsedTimer &mcpTraceTimer = ctx.traceTimer;
    // Written to by the etag short-circuit below, then read by recordDispatch;
    // the mutation never needs to escape this function.
    QString dispatchResult = ctx.dispatchResult;

    QJsonObject result;
    QJsonObject error;
    bool haveResult = false;
    // ANTS-1357 — populate cache on miss-success. INV-5
    // exclusions enforced inside maybeInsertIdempotentReadCache.
    if (toolHandled && cacheable && !cachedHit) {
        maybeInsertIdempotentReadCache(
            toolName, argsObj, reply.cacheBody);
    }
    if (toolHandled) {
        result = toolCallResult(reply.wrapped, reply.refusalCode);
        if (reply.etagUnchanged) {
            dispatchResult = QStringLiteral("etag_unchanged");
        }
        // ANTS-1284 — record dispatch (token_usage +
        // mcp_trace). ANTS-1402-INV-3: now teed through
        // a single recordDispatch hook so both observers
        // see byte-identical numbers.
        // ANTS-1355: wrap delta + dispatch latency captured
        // once at the dispatch site and forwarded verbatim.
        // ANTS-4457 — a handler that ran and refused never reached
        // dispatchResult, so the call was booked as a success and the
        // trace said "ok". Only override the default: a dispatcher-level
        // result (etag_unchanged, rate_limited, …) already describes the
        // call more specifically than the envelope does.
        if (dispatchResult == QLatin1String("ok")) {
            if (!reply.refusalCode.isEmpty()) dispatchResult = reply.refusalCode;
        }
        const qint64 argBytes  = reply.argBytes;
        const qint64 outBytes  = reply.outBytes;
        const qint64 wrapBytes = reply.wrapBytes;
        const qint64 durUs     = mcpTraceTimer.nsecsElapsed() / 1000;
        // dispatchResultIsSuccess decides which results count as success.
        // ANTS-4659 — the survey of what callers send undeclared. A call
        // ants-mcpd forwards and the terminal answers is finished only in the
        // terminal, so with both processes recording each call lands once.
        // Above recordDispatch: mcp_rate_limit INV-10 pins haveResult as the
        // statement right after it.
        if (!m_undeclaredArgsLedgerPath.isEmpty() &&
            !ctx.ignoredArgKeys.isEmpty()) {
            McpUndeclaredArgs::appendRecord(m_undeclaredArgsLedgerPath,
                                            toolName, ctx.ignoredArgKeys);
        }
        recordDispatch(toolName, argsObj, argBytes,
                       /*rawBytes=*/ctx.requestBytes, outBytes,
                       wrapBytes, durUs, cachedHit,
                       dispatchResult);
        haveResult = true;
    } else {
        // JSON-RPC application error: tool not found or provider missing.
        // ANTS-5485 — a removed verb is told what replaced it.
        const QJsonObject removed = mcp::removedVerbError(toolName);
        if (!removed.isEmpty()) {
            error = removed;
        } else {
            error["code"] = -32602; // Invalid params
            error["message"] = QString("Unknown tool: %1").arg(toolName);
        }
        // ANTS-1402-INV-4 — failure-branch hook routes through
        // recordDispatch with result="tool_not_found". INV-12 of
        // ANTS-1360: resp_bytes=0 and a non-negative duration_us
        // reach the mcp_trace ring.
        const qint64 argBytes = QJsonDocument(argsObj)
            .toJson(QJsonDocument::Compact).size();
        const qint64 durUs = mcpTraceTimer.nsecsElapsed() / 1000;
        recordDispatch(toolName, argsObj, argBytes,
                       /*rawBytes=*/ctx.requestBytes,
                       /*outBytes=*/0,
                       /*wrapBytes=*/0,
                       durUs,
                       /*cachedHit=*/false,
                       QStringLiteral("tool_not_found"));
    }
    sendMcpResponse(ctx.socket, ctx.requestId,
                    haveResult ? &result : nullptr,
                    haveResult ? nullptr : &error);
}

// ANTS-2132 — the JSON-RPC envelope write, shared by every method so the
// deferred path cannot drift from the synchronous one.
void ClaudeIntegration::sendMcpResponse(const QPointer<McpReplyChannel> &socket,
                                        const QJsonValue &requestId,
                                        const QJsonObject *result,
                                        const QJsonObject *error) {
    // ANTS-2101 — the peer can disconnect mid-dispatch, freeing this socket
    // via disconnected -> deleteLater. Bail before touching a dangling
    // pointer. ANTS-2132 widens that window: the reply may now be written a
    // whole verb-duration after the request arrived. Read the QPointer once:
    // each `->` re-reads it, and GCC then warns the later calls may see null.
    McpReplyChannel *const channel = socket.data();
    if (!channel || !channel->isOpen()) return;
    // Notifications (no id) must NOT receive a response per JSON-RPC 2.0.
    if (requestId.isUndefined() || requestId.isNull()) {
        channel->finish();
        return;
    }
    QJsonObject envelope;
    envelope["jsonrpc"] = "2.0";
    envelope["id"] = requestId;
    if (result)     envelope["result"] = *result;
    else if (error) envelope["error"]  = *error;
    QByteArray resp = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    // ANTS-1769 — '\n' end-of-reply terminator; compact JSON carries no raw
    // newline, so it is an unambiguous "reply complete" marker.
    resp.append('\n');
    channel->reply(resp);
}

// ANTS-5089 — the JSON-RPC error for a complete request line the MCP socket
// cannot dispatch: -32700 when it does not parse, -32600 when it parses but is
// not an object. Written here, not through sendMcpResponse, which sends nothing
// for a null id. Same terminator (ANTS-1769).
static void writeMcpRequestError(McpReplyChannel *socket,
                                 const QJsonParseError &parseError) {
    QJsonObject rpcError;
    if (parseError.error != QJsonParseError::NoError) {
        rpcError["code"]    = -32700;
        rpcError["message"] = QStringLiteral("Parse error: %1")
                                  .arg(parseError.errorString());
    } else {
        rpcError["code"]    = -32600;
        rpcError["message"] =
            QStringLiteral("Invalid Request: expected a JSON object");
    }
    QJsonObject envelope;
    envelope["jsonrpc"] = "2.0";
    envelope["id"]      = QJsonValue(QJsonValue::Null);
    envelope["error"]   = rpcError;
    if (!socket || !socket->isOpen()) return;
    socket->reply(QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n');
}

void ClaudeIntegration::onMcpConnection() {
    while (m_mcpServer->hasPendingConnections()) {
        QLocalSocket *socket = m_mcpServer->nextPendingConnection();
        // ANTS-5144 § 2.5 — take the socket from the shared server.
        socket->setParent(this);
        // ANTS-1151 — same SO_PEERCRED + idle-timeout pattern as
        // onHookConnection. MCP socket carries higher-leverage
        // verbs (filesystem reads, git status, environment),
        // peer-cred check is more important here.
        // ANTS-1797 — fail CLOSED: an unavailable socket fd means the peer
        // UID cannot be verified, so the connection must be refused rather
        // than served unauthenticated. (A bare `if (fd >= 0)` guard would
        // skip the check entirely on fd<0.)
        const qintptr fd = socket->socketDescriptor();
        bool peerVerified = false;
        if (fd >= 0) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            if (::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                             SO_PEERCRED, &cred, &len) == 0 &&
                len == sizeof(cred) && cred.uid == ::getuid()) {
                peerVerified = true;
            }
        }
        if (!peerVerified) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        if (!ants::admitLiveConnection(this, socket, m_liveMcpConnections)) continue;
        QTimer *idleTimer = new QTimer(socket);
        idleTimer->setSingleShot(true);
        idleTimer->setInterval(5000);
        connect(idleTimer, &QTimer::timeout, socket,
                [socket]() { socket->abort(); });
        idleTimer->start();
        // ANTS-5089 — one request per line: the request is the bytes before
        // the first newline. Until one arrives, scan only the bytes that just
        // came in and wait; nothing is parsed, so a slow sender no longer
        // costs a re-parse of the whole buffer on every readyRead. ANTS-2216 —
        // nor a copy of it: the buffer lives in the lambda, not a property.
        auto buf = std::make_shared<QByteArray>();
        socket->setProperty("_handled", false);
        // ANTS-4932 — the reply channel for this connection: one reply, then
        // close. A child of the socket, so it dies with it.
        auto *channel = new McpReplyChannel(
            [socket](const QByteArray &line) {
                socket->write(line);
                socket->flush();
                socket->disconnectFromServer();
            },
            [socket] { socket->disconnectFromServer(); },
            [socket] { return socket->state() == QLocalSocket::ConnectedState; },
            socket);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, idleTimer, channel, buf]() {
            if (socket->property("_handled").toBool()) return;
            const qsizetype scanFrom = buf->size();
            *buf += socket->readAll();
            if (buf->size() > kMaxSocketRequestBytes) { socket->disconnectFromServer(); return; }
            const qsizetype newline = buf->indexOf('\n', scanFrom);
            if (newline < 0) return;
            // ANTS-4932 — a complete request is in hand. Mark it handled and
            // stop the 5 s slow-loris idle timer BEFORE dispatching (ANTS-2101:
            // a dispatch can run a nested event loop, and a still-armed timer
            // would abort -> deleteLater this socket under it), then hand the
            // line to the shared entry point.
            socket->setProperty("_handled", true);
            idleTimer->stop();
            handleMcpLine(buf->left(newline), channel);
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

// ANTS-4932 — the one entry point for a complete request line. The socket
// path above and ants-mcpd's stdin reader both call it.
void ClaudeIntegration::handleMcpLine(const QByteArray &line, McpReplyChannel *out) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (!doc.isObject()) {
        writeMcpRequestError(out, parseError);
        return;
    }
    handleMcpRequest(doc, line, QPointer<McpReplyChannel>(out));
}

// ANTS-4919 — the schema-property helpers more than one piece of the
// tools/list branch uses, promoted from local lambdas (ANTS-1677 § 2.4).
// Declared in claudeintegration_internal.h.
namespace claudeintegrationdetail {

// ANTS-1520 — shared schema property for tools that
// anchor on caller_cwd. Most project-scoped read and
// write tools REQUIRE this (refused at the dispatcher
// with code `caller_cwd_required` when absent).
//
// ANTS-4457 — the SEVEN verbs classified TabSpecific are
// get_scrollback, get_text, recent_errors,
// last_selection, get_last_command, get_environment and
// get_cwd. They are a refusal gate too, not a routing
// hint: ANTS-1415 Phase 3b removed the focused-tab
// fallback and refuses `tab_or_cwd_required` when
// neither caller_cwd nor a usable `tab` index is
// present, because falling back to whichever tab is
// focused is the cross-tenant leak ANTS-1404 closed for
// the Required tools. Three of the seven — get_text,
// recent_errors, last_selection — take `tab` as the
// alternative key; on the other four the handler
// ignores it, so it is not a routing key there.
//
// This comment is the enumeration; the wire description
// below stays short because it rides on nearly every
// tool in the catalogue.
// tests/features/caller_cwd_schema_truth keeps the two
// honest against the classifier.
// Uniform schema opener so consumers see one
// canonical statement instead of N variants
// (ANTS-1520 spec: "per-tool docstring opens with
// the same line"). Original ANTS-1391 rationale
// (don't return project A's data when a session is
// asking about project B) drove the uniform-Required
// upgrade.
// Write tools use it too, despite "Read" in the name;
// the name stays because dated specs cite it.
QJsonObject makeCallerCwdReadProp() {
    QJsonObject p;
    p["type"] = "string";
    p["description"] = QStringLiteral(
        "Your $PWD. Required by the dispatcher for "
        "project-scoped read and write tools "
        "(refuses with code `caller_cwd_required` "
        "when absent — ANTS-1520). The per-tab "
        "terminal-state verbs refuse "
        "`tab_or_cwd_required` instead (ANTS-1415); "
        "three of them (get_text, recent_errors, "
        "last_selection) also accept a `tab` index. "
        "There is no focused-tab fallback.");
    return p;
}

// ANTS-1409 — canonical "Pass caller_cwd to anchor"
// suffix used verbatim by the get_last_command and
// get_environment descriptions.
// The helper keeps the phrasing in one place so
// future MCP tools opt in via `+ callerCwdSuffix()`
// rather than re-typing it. INV-4 of the spec
// explicitly excludes get_scrollback and get_text —
// those carry tool-specific phrasing the canonical
// short suffix doesn't.
QString callerCwdSuffix() {
    return QStringLiteral(
        "Pass `caller_cwd` to anchor to your tab (ANTS-1392).");
}

// ANTS-1499 — `etag_match` input prop for read tools
// that opt into the "304 Not Modified" pattern (the
// tools allowlisted in isEtagSupportedTool()); the same
// schema fragment is reused for each so the field
// description stays in lockstep.
QJsonObject makeEtagMatchProp() {
    QJsonObject p;
    p["type"] = "string";
    p["description"] = QStringLiteral(
        "Optional. Server-issued etag from a prior "
        "call. If the server's current etag equals "
        "this value, the response is short-circuited "
        "to {ok:true, unchanged:true, etag:\"<same>\"} "
        "— saves the full response body. Otherwise the "
        "current response carries a fresh `etag` field "
        "for the next call (ANTS-1499 \"304 Not "
        "Modified\" pattern). ANTS-4859 — the etag "
        "hashes the whole reply, so send the same "
        "arguments as the call that issued it: any "
        "argument that changes the reply (a mode, a "
        "cap, an include_* flag) changes the etag. "
        "`fields` is the exception, applied after the "
        "etag.");
    return p;
}

// find_definition's and find_caller's `lang`: the language
// families their scan supports (ANTS-2150 / ANTS-3558).
QJsonObject makeSymbolLangProp() {
    QJsonObject p;
    p["type"]    = "string";
    p["enum"]    = QJsonArray{
        QStringLiteral("auto"), QStringLiteral("cpp"),
        QStringLiteral("py"), QStringLiteral("lua"),
        QStringLiteral("sh"), QStringLiteral("generic"),
        QStringLiteral("cmake"), QStringLiteral("glsl")};   // cmake: ANTS-5544
    p["default"] = "auto";
    p["description"] = QStringLiteral(
        "Restrict the scan to one language family. "
        "\"auto\" (default) scans every supported family "
        "(cpp, py, lua, sh, generic, cmake, glsl).");
    return p;
}

// ANTS-1720 — `fields` projection input prop: the response
// carries only the named top-level fields. ANTS-4524 — the
// property is now injected into EVERY verb's schema in the
// tools/list loop below, with a terse description. This full
// one is what the high-volume read verbs keep, because they
// are where a caller is most likely to reach for it; the
// injection skips a schema that already declares its own.
QJsonObject makeFieldsProp() {
    QJsonObject p;
    p["type"] = "array";
    QJsonObject items;
    items["type"] = "string";
    p["items"] = items;
    p["description"] = QStringLiteral(
        "Optional. Return only these top-level response "
        "fields (e.g. [\"bullets\"], [\"branch\","
        "\"files\"]). A name the envelope does not carry is "
        "listed in `fields_unmatched` (ANTS-4567), emitted only "
        "when non-empty — so a narrowed reply that comes back "
        "nearly empty says WHY, and a correct probe of an "
        "inapplicable field is distinguishable from a misspelling "
        "or from the argument being dropped. ANTS-4930: it "
        "arrives with `fields_available`, the top-level keys the "
        "envelope DOES carry, because `fields_unmatched` alone "
        "collapses three facts into one array — a name this verb "
        "never carries, one it carries only on another backend, "
        "and one it carries only when there is something to "
        "report. Read the two together: a misspelling shows up as "
        "the right name sitting in `fields_available`, and for "
        "the others the envelope's real shape is there to branch "
        "on instead of an absence. An all-unknown list "
        "yields that field alone. Omit for the full "
        "payload — fully backwards-compatible (ANTS-1720). "
        "To keep the etag for a follow-up 304 call, "
        "include \"etag\" in the list: the etag is "
        "computed on the unfiltered body, so a narrowed "
        "call still short-circuits when state is "
        "unchanged (composes with etag_match).");
    return p;
}

// ANTS-2091 — `compact` opt-in flag, added alongside
// `fields` on the same read tools. When true, the response
// is stripped of dead-weight fields (null / false / "" /
// [] / {}) the model never reads; protected keys (ok, code,
// error, etag, found, unchanged) always survive.
QJsonObject makeCompactProp() {
    QJsonObject p;
    p["type"] = "boolean";
    p["default"] = false;
    p["description"] = QStringLiteral(
        "Optional (ANTS-2091). When true, drop dead-weight "
        "fields (null / false / empty string / empty array "
        "/ empty object) the model never reads — recursively, "
        "including per-element empties. Protected keys (ok, "
        "code, error, etag, found, unchanged) always survive. "
        "Absent ⟺ default, so a reader that treats a dropped "
        "field as its zero-value sees no change; omit it when "
        "you must distinguish empty from absent. Composes "
        "with fields= and etag_match.");
    return p;
}

// ANTS-2218 — `raw` opt-in flag on the content-read verbs
// (read_region / read_regions / workspace_search). When true
// the body is returned VERBATIM inside an unforgeable nonce
// frame instead of the default scrub that neutralises literal
// </ants_mcp_data> and `<!--`/`-->` markers — so an agent
// reading frame-sensitive source (this file, a spec, an HTML/
// markdown file with comments) gets true bytes to Edit from.
QJsonObject makeRawProp() {
    QJsonObject p;
    p["type"] = "boolean";
    p["default"] = false;
    p["description"] = QStringLiteral(
        "Optional (ANTS-2218). When true, return the file "
        "content VERBATIM. The default framing neutralises any "
        "literal MCP envelope close-tag and `<!--`/`-->` "
        "comment markers in the bytes (lossy, to protect the "
        "response frame) — which corrupts an Edit/apply_edits "
        "built from a file that itself contains those tokens "
        "(this MCP source, a spec, HTML/markdown with "
        "comments). Raw mode wraps the bytes in a unique "
        "per-call frame instead, so they round-trip unmodified. "
        "Set it when you will Edit from the output; omit "
        "otherwise.");
    return p;
}

// ANTS-6125 — what the verbs that walk a project's source
// files share: function_sizes, source_scrapes, comment_claims
// and duplicate_blocks.
QJsonObject makeWalkCallerCwdProp() {
    QJsonObject p;
    p["type"] = "string";
    p["description"] = QStringLiteral(
        "Your $PWD. REQUIRED — the project the paths resolve in.");
    return p;
}
QJsonObject makeWalkPathsProp(const QString &description) {
    QJsonObject item;
    item["type"] = "string";
    QJsonObject p;
    p["type"] = "array";
    p["items"] = item;
    p["description"] = description;
    return p;
}
QJsonObject makeWalkIncludeProp(const QString &files) {
    QJsonObject p;
    p["type"] = "boolean";
    p["description"] = QStringLiteral("Also read %1 (default false).").arg(files);
    return p;
}
// ANTS-2227 — uniform `dry_run` flag for mutating verbs. When
// true the verb computes the would-be result envelope (carrying
// dry_run:true) WITHOUT writing to disk. The per-verb
// descriptions on roadmap_log / changelog_log / spec_log
// (ANTS-2077 / 2136) predate this factory and keep their
// tailored copies; new write verbs share this one.
QJsonObject makeDryRunProp() {
    QJsonObject p;
    p["type"] = "boolean";
    p["default"] = false;
    p["description"] = QStringLiteral(
        "Optional (ANTS-2227). When true, return the would-be "
        "result (carrying `dry_run:true`) WITHOUT writing to "
        "disk — a free pre-flight that shares the real write "
        "path up to the point of no return, so the preview "
        "cannot drift from the actual result on anything it "
        "reaches. ANTS-4548 — it stops SHORT of the commit and "
        "of the write to disk, so a green preview is evidence "
        "the request is valid, never that the write landed. "
        "Defaults false.");
    return p;
}

// ANTS-2090 — `encoding` selector, declared on the
// list-shaped read verbs. "tabular" packs each eligible
// top-level array-of-objects into a columnar
// {__cols__,__rows__} form (one header row + one value-row
// per element), 30–60% smaller on big lists. Opt-in only —
// the caller must decode it — so there is no session default.
QJsonObject makeEncodingProp() {
    QJsonObject p;
    p["type"] = "string";
    QJsonArray enumVals;
    enumVals.append("json");
    enumVals.append("tabular");
    p["enum"] = enumVals;
    p["default"] = "json";
    p["description"] = QStringLiteral(
        "Optional (ANTS-2090). \"tabular\" packs each "
        "top-level array-of-objects into a columnar "
        "{__cols__:[names], __rows__:[[values]]} form — "
        "the per-row keys are dropped, so big list replies "
        "(bullets[], matches[], symbols[]) are 30–60% "
        "smaller. Decode by zipping __cols__ with each "
        "__rows__ entry (a null cell = key absent on that "
        "element). Opt-in because you must decode it; "
        "never made bigger than plain JSON. Default "
        "\"json\" (unchanged).");
    return p;
}

}  // namespace claudeintegrationdetail

using namespace claudeintegrationdetail;

// ANTS-4932 — the request body below keeps the indentation it had inside
// onMcpConnection's readyRead lambda, so the move changed no line of it.
// `buf` is the request line; `guard` is its reply channel.
void ClaudeIntegration::handleMcpRequest(const QJsonDocument &doc,
                                         const QByteArray &buf,
                                         const QPointer<McpReplyChannel> &guard) {
    {
        {
            QJsonObject request = doc.object();
            QString method = request.value("method").toString();
            QJsonValue reqId = request.value("id");

            // Build either `result` (success) or `error` (failure). The
            // envelope ({jsonrpc, id, result/error}) is added at the end.
            QJsonObject result;
            QJsonObject error;
            bool haveResult = false;

            if (method == "initialize") {
                // ANTS-1284 — reset per-tool dispatch counters on
                // session-start handshake. See docs/specs/ANTS-1284-token-usage-mcp-telemetry-tool.md.
                // ANTS-3572 — fold the previous session into the persisted
                // aggregate before clearing (endTokenSession emits first).
                endTokenSession();
                QJsonObject caps;
                caps["tools"] = QJsonObject();
                QJsonObject serverInfo;
                serverInfo["name"] = m_serverName;  // ANTS-4932 — "ants-mcpd" there
                serverInfo["version"] = QStringLiteral(ANTS_VERSION);
                // ANTS-1952 — build identity so a caller can detect a
                // ship-vs-live binary gap (same SemVer, rebuilt with a fix).
                // SemVer alone can't distinguish it; the git SHA can. Lets a
                // session compare against `git log` and flag "server predates
                // commit X" instead of chasing stale telemetry (cf. the
                // ANTS-1632/1903/1947 stale-binary investigations).
                // ANTS-3582: ANTS_BUILD_* are extern const char[] (build_info_values.cpp),
                // so read via fromLatin1 — not compile-time literals.
                serverInfo["build_commit"] = QString::fromLatin1(ANTS_BUILD_COMMIT);
                serverInfo["build_date"] = QString::fromLatin1(ANTS_BUILD_DATE);
                serverInfo["build_time"] = QString::fromLatin1(ANTS_BUILD_TIME);
                serverInfo["build_type"] = QString::fromLatin1(ANTS_BUILD_TYPE);
                result["protocolVersion"] = "2025-11-25";
                result["capabilities"] = caps;
                result["serverInfo"] = serverInfo;
                haveResult = true;
            } else if (method == "tools/list") {
                QJsonArray tools;

                appendToolSchemasTerminal(tools);

                appendToolSchemasCodeNav(tools);

                appendToolSchemasDocs(tools);

                appendToolSchemasOrientation(tools);
                appendToolSchemasStore(tools);
                // ANTS-5734 — trace: a symbol's definitions, callers,
                // references, tests, documents, spec invariants and roadmap
                // items in one call. Listed only by a process that registered
                // it (INV-13): ants-mcpd does, the terminal cannot, since it
                // does not link the parser. Every property below is read by
                // trace::serve (ANTS-4621).
                if (m_toolProviders.count(QStringLiteral("trace"))) {
                    QJsonObject t;
                    t["name"] = "trace";
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of grep + Read to learn where a symbol is "
                        "defined, who calls it, and which tests and documents "
                        "name it.");
                    t["description"] = QStringLiteral(
                        "One call for a symbol: definitions with end_line, depth-1 "
                        "callers, references by kind, tests, doc lines, spec "
                        "invariants and roadmap items. path= traces a file instead. "
                        "Syntactic; not_covered says what it missed. Refusals: "
                        "bad_args, bad_path, no_project. caller_cwd Required.");
                    t["detail"] = QStringLiteral(
                        "One call for a symbol: definitions (file, line, end_line, "
                        "signature, kind, scope, is_declaration), depth-1 callers "
                        "with the calling line, every other reference by kind "
                        "(import, code, comment, string_or_config, test, unparsed), "
                        "the test files that hit it with ctest patterns, doc lines "
                        "with their nearest heading, spec invariants and roadmap "
                        "items naming it. With path instead of symbol: the tests, "
                        "docs and config that name that file. Matching is syntactic "
                        "(tree-sitter queries, no type resolution), so precision is "
                        "\"syntactic\". not_covered lists languages with no "
                        "grammar, files too large or timed out, an ambiguous "
                        "basename, an unreadable roadmap or spec, and each "
                        "language's limits. Rows page under max_bytes: pass "
                        "next_offset back as offset. depth above 1 and "
                        "include:[\"callees\"] refuse bad_args until ship 2. "
                        "Refusals: bad_args, bad_path, no_project. caller_cwd "
                        "Required. Spec: docs/specs/ANTS-5734-trace-verb.md.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    const auto str = [](const char *d) {
                        return QJsonObject{{"type", "string"},
                                           {"description", QString::fromUtf8(d)}};
                    };
                    props["caller_cwd"] = str("Your $PWD (required); picks the project.");
                    props["symbol"] = str(
                        "A name, or up to four joined by :: or . (Foo::bar); "
                        "the leading parts narrow definitions, callers and references.");
                    props["path"] = str(
                        "A file to trace instead of a symbol: what tests, docs and "
                        "config name it.");
                    props["lang"] = str(
                        "Parse only this language id (cpp, python, ...).");
                    {
                        QJsonArray members;
                        for (const char *m : {"definitions", "callers", "references",
                                              "tests", "docs", "spec_invariants",
                                              "roadmap_items"})
                            members.append(QString::fromLatin1(m));
                        props["include"] = QJsonObject{
                            {"type", "array"},
                            {"items", QJsonObject{{"type", "string"}, {"enum", members}}},
                            {"description", QStringLiteral(
                                 "Only these sections; default all.")}};
                    }
                    props["depth"] = QJsonObject{
                        {"type", "integer"}, {"minimum", 1}, {"maximum", 3},
                        {"default", 1},
                        {"description", QStringLiteral("Caller depth; only 1 is served yet.")}};
                    props["max_bytes"] = QJsonObject{
                        {"type", "integer"}, {"minimum", 1},
                        {"description", QStringLiteral("Reply cap (default 512 KiB).")}};
                    props["offset"] = QJsonObject{
                        {"type", "integer"}, {"minimum", 0},
                        {"description", QStringLiteral(
                             "Row cursor: pass back next_offset.")}};
                    schema["properties"] = props;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                appendToolSchemasReports(tools);

                // ANTS-1354 — default `version: "1.0"` on every tool
                // descriptor unless one already set its own. SemVer-
                // of-tools policy:
                //   * MAJOR — wire-format break (response shape
                //     changes incompatibly, arg semantics flip,
                //     refusal-code dropped). Caller code that worked
                //     against the previous MAJOR cannot be expected
                //     to still parse.
                //   * MINOR — additive change (new optional arg, new
                //     response field, new opt-in flag). Old callers
                //     keep working.
                //   * PATCH — bug fix only; no observable schema
                //     change.
                // Bumping is per-tool: an additive change to
                // `roadmap_query` (e.g. ANTS-1425 narrator opt-in)
                // bumps `roadmap_query.version` to "1.1" without
                // touching siblings. Callers dispatch on this field
                // to decide whether their parsing path still applies.
                //
                // ANTS-1505 — per-tool budget hints. `typical_token_cost`
                // is the token count a caller should expect on the
                // common path; `worst_case_tokens` is the cap a fresh
                // call against a large project might hit (after
                // pagination + truncate). Both are conservative
                // estimates rounded to ≤2 sig-fig; they're hints, not
                // contracts. Callers can use them to size cold-start
                // discovery or to decide between two tools that
                // expose overlapping data.
                auto tokenCostFor = [](const QString &name)
                        -> std::pair<int, int> {
                    // Buckets: (typical, worst). A verb with no row takes
                    // the default below, an estimate nobody measured.
                    static const QHash<QString, std::pair<int, int>> kCosts = {
                        // Control-plane / cheap fixed responses.
                        {QStringLiteral("get_session_info"),  {100,  400}},
                        {QStringLiteral("get_cwd"),           {80,   200}},
                        {QStringLiteral("caller_cwd_info"),   {150,  400}},
                        {QStringLiteral("tab_list"),          {200,  800}},
                        {QStringLiteral("tool_info"),         {500,  2000}},
                        {QStringLiteral("token_usage"),       {600,  2500}},
                        {QStringLiteral("mcp_trace"),         {800,  3000}},
                        // Terminal-state reads.
                        {QStringLiteral("get_last_command"),  {600,  3000}},
                        {QStringLiteral("get_environment"),   {300,  800}},
                        {QStringLiteral("get_scrollback"),    {1500, 8000}},
                        {QStringLiteral("get_text"),          {1200, 6000}},
                        // Scrollback error extraction (ANTS-1301).
                        {QStringLiteral("recent_errors"),     {800,  4000}},
                        // Selection read-through (ANTS-1312).
                        {QStringLiteral("last_selection"),    {200,  2000}},
                        // Topic-to-files discovery (ANTS-1636).
                        {QStringLiteral("find_sources"),      {600,  3500}},
                        // Documentation map (ANTS-2139): summary is the
                        // ~80 B/doc light list; topic/doc_path bodies smaller.
                        {QStringLiteral("docs_index"),        {800,  6000}},
                        // ANTS-3601 — doc_integrity: findings list, usually
                        // small (most docs are clean); grows with breakage.
                        {QStringLiteral("doc_integrity"),     {500,  4000}},
                        // ANTS-3636 — doc_citations: one entry per citation
                        // plus its text; a dense spec is the upper end.
                        {QStringLiteral("doc_citations"),     {900,  8000}},
                        // ANTS-3661 — doc_symbols: one entry per candidate
                        // occurrence across the walked corpus.
                        {QStringLiteral("doc_symbols"),       {900,  8000}},
                        // ANTS-3662 — spec_lint: findings are sparse on a
                        // conforming corpus; line_count adds one row per spec.
                        {QStringLiteral("spec_lint"),         {600,  5000}},
                        // ANTS-4108 — spec_conformance: findings are sparse,
                        // but observations[] is one row per executed case, so
                        // the ceiling scales with max_cases rather than defects.
                        {QStringLiteral("spec_conformance"),  {700,  9000}},
                        // ANTS-3660 — doc_dedup: one entry per pair PLUS one
                        // per cluster, and a corpus-wide walk is the upper end
                        // (measured: 275 pairs / 128 clusters over docs/).
                        {QStringLiteral("doc_dedup"),         {1200, 12000}},
                        // ANTS-3663 — doc_lint composes all six checkers, so
                        // its ceiling is their union over one walk; max_findings
                        // is what a caller bounds it with.
                        {QStringLiteral("doc_lint"),          {1500, 16000}},
                        // Repo / docs.
                        {QStringLiteral("roadmap_query"),     {1700, 12000}},
                        {QStringLiteral("roadmap_log"),       {200,  600}},
                        // ANTS-3855 — roadmap_migrate: counts + a bounded
                        // notes[] (default 200 entries), no roadmap text echoed
                        // back. The upper end is a corpus project whose source
                        // raises the note cap.
                        {QStringLiteral("roadmap_migrate"),   {300,  4000}},
                        {QStringLiteral("project_layout"),    {600,  2000}},
                        // ANTS-2161 — project_settings: small detect/write envelope.
                        {QStringLiteral("project_settings"),  {300,  1500}},
                        // ANTS-2093 — project_query: aggregate answer, occasionally
                        // a small list; result_cap_bytes (64 KiB default) bounds it.
                        {QStringLiteral("project_query"),     {400,  4000}},
                        {QStringLiteral("session_memory"),    {200,  1000}},
                        // ANTS-1883 — composer of three large reads + ANTS-1922
                        // active_bullets (top-20 headline_only, ~2 KB); bucket
                        // = sum of constituents' worst case + bullet overhead.
                        {QStringLiteral("session_orient"),    {2500, 17000}},
                        {QStringLiteral("workflow_state"),    {200,  1000}},
                        {QStringLiteral("workspace_search"),  {1500, 10000}},
                        {QStringLiteral("file_outline"),      {800,  4000}},
                        {QStringLiteral("git_state"),         {700,  4000}},
                        {QStringLiteral("subsystem"),         {400,  1500}},
                        // Plan + verify.
                        {QStringLiteral("verify_changes"),    {600,  3000}},
                        // Audit suite.
                        {QStringLiteral("audit_run"),         {4000, 25000}},
                        {QStringLiteral("last_audit_summary"),{600,  2500}},
                        // Cold-eyes.
                        {QStringLiteral("cold_eyes_partition"),    {1000, 4000}},
                        {QStringLiteral("cold_eyes_brief"),        {1500, 6000}},
                        // Indie-review.
                        {QStringLiteral("indie_review_partition"),    {1500, 6000}},
                        {QStringLiteral("indie_review_corroborate"),  {1500, 6000}},
                        {QStringLiteral("indie_review_dispatch"),     {3000, 12000}},
                        // Test-audit.
                        {QStringLiteral("test_audit_partition"),       {1500, 6000}},
                        {QStringLiteral("test_audit_brief"),           {2000, 8000}},
                        // Spec-aware (ANTS-1309 + ANTS-1308).
                        {QStringLiteral("spec_query"),         {500,  2500}},
                        {QStringLiteral("invariant_check"),    {800,  4000}},
                        // Build/test cache (ANTS-1299 + ANTS-1300).
                        {QStringLiteral("build_status"),       {500,  2000}},
                        {QStringLiteral("test_results"),       {800,  3000}},
                        // Symbol queries (ANTS-1303).
                        {QStringLiteral("find_definition"),    {600,  2500}},
                        {QStringLiteral("find_caller"),        {800,  4000}},
                        // Shape matcher (ANTS-1305).
                        {QStringLiteral("similar_code"),       {600,  2500}},
                        // Task-start context composers (ANTS-1306 + ANTS-1307).
                        {QStringLiteral("task_priors"),        {1200, 6000}},
                        {QStringLiteral("project_conventions"),{400,  1500}},
                        // Focused test runner (ANTS-1302).
                        {QStringLiteral("focused_test"),       {800,  4000}},
                        // ANTS-3745 — build_target_for: one small envelope,
                        // a CMake parse and at most two file reads.
                        {QStringLiteral("build_target_for"),   {300,  1200}},
                    };
                    const auto it = kCosts.find(name);
                    if (it != kCosts.end()) return it.value();
                    return {500, 2500};  // conservative default
                };
                // ANTS-1518 — discoverability prefix-tag. Defined here
                // (outer tools/list scope) so both the post-build loop
                // and the lite-shape branch reuse the same mapping.
                // Same kind-buckets the lite-shape kind: field uses,
                // emitted as a `[<kind>] ` prefix on every full-shape
                // description so a session surfacing tools/list can
                // grep by surface family without parsing every
                // descriptor.
                // ANTS-1567 — every tool has a non-"other" prefix, named
                // after the surface family a session would grep tools/list
                // for. `mcp-state` is server-side per-cwd state, and
                // `caller_cwd_info` is `meta`: a diagnostic, not a pty read.
                auto kindForName = [](const QString &name) -> QString {
                    if (name.startsWith(QStringLiteral("cold_eyes_")))
                        return QStringLiteral("cold-eyes");
                    if (name.startsWith(QStringLiteral("indie_review_")))
                        return QStringLiteral("indie-review");
                    if (name.startsWith(QStringLiteral("test_audit_")))
                        return QStringLiteral("test-audit");
                    if (name.startsWith(QStringLiteral("roadmap_")))
                        return QStringLiteral("roadmap");
                    // ANTS-1548 — changelog_log (writer) + ANTS-3533
                    // changelog_query (reader) are the CHANGELOG siblings
                    // of the roadmap family.
                    if (name.startsWith(QStringLiteral("changelog_")))
                        return QStringLiteral("roadmap");
                    if (name == QLatin1String("audit_run") ||
                        name == QLatin1String("last_audit_summary") ||
                        // ANTS-3396 — audit_poll: poll an async audit_run job.
                        name == QLatin1String("audit_poll") ||
                        // ANTS-2129 — audit_falsepos_log: write side of the
                        // false-positive ledger, audit-family.
                        name == QLatin1String("audit_falsepos_log") ||
                        // ANTS-1713 — audit_dismiss: write side of the
                        // fingerprint-keyed learned-FP ledger.
                        name == QLatin1String("audit_dismiss"))
                        return QStringLiteral("audit");
                    if (name == QLatin1String("verify_changes"))
                        return QStringLiteral("verify");
                    if (name == QLatin1String("git_state"))
                        return QStringLiteral("git");
                    if (name == QLatin1String("workspace_search") ||
                        // ANTS-3716 — cited_by: caller_cwd-Required,
                        // path-validated tree search (workspace_search family,
                        // whose rg runner it shares).
                        name == QLatin1String("cited_by") ||
                        name == QLatin1String("file_outline") ||
                        // ANTS-1855 — read_log: caller_cwd-Required,
                        // path-validated file reader (file_outline family).
                        name == QLatin1String("read_log") ||
                        // ANTS-2021 — read_region: caller_cwd-Required,
                        // path-validated slice reader (file_outline family).
                        name == QLatin1String("read_region") ||
                        // ANTS-2219 — read_regions: batched slice reader.
                        name == QLatin1String("read_regions") ||
                        // ANTS-2094 — read_spill: re-read an offloaded result
                        // by content-addressed handle (global cache reader).
                        name == QLatin1String("read_spill") ||
                        // ANTS-2022 — apply_edits: caller_cwd-Required,
                        // path-validated batch file editor (workspace write).
                        name == QLatin1String("apply_edits") ||
                        // ANTS-1636 — find_sources: project-scoped
                        // src/+tests/ topic walker.
                        name == QLatin1String("find_sources") ||
                        // ANTS-3368 — co_change_family: project-scoped
                        // repo-wide co-change site scan.
                        name == QLatin1String("co_change_family") ||
                        // ANTS-1637 — codebase_index: project-scoped
                        // structural-map reader.
                        name == QLatin1String("codebase_index") ||
                        // ANTS-2139 — docs_index: project-scoped
                        // documentation-map reader.
                        name == QLatin1String("docs_index") ||
                        // ANTS-3601 — doc_integrity: project-scoped
                        // doc-consistency reader.
                        name == QLatin1String("doc_integrity") ||
                        // ANTS-3636 — doc_citations: project-scoped
                        // citation-resolution reader, doc_integrity's sibling.
                        name == QLatin1String("doc_citations") ||
                        // ANTS-5502 — quotation_check: does each quoted
                        // sentence occur in the file it names.
                        name == QLatin1String("quotation_check") ||
                        name == QLatin1String("version_check") ||   // ANTS-5510
                        name == QLatin1String("dependency_inventory") ||   // ANTS-5515
                        name == QLatin1String("ci_report") ||   // ANTS-6399
                        name == QLatin1String("doc_symbols") ||
                        name == QLatin1String("spec_lint") ||   // ANTS-3662
                        // ANTS-4108 — spec_conformance: spec_lint's executable
                        // sibling; same project-scoped document reader family.
                        name == QLatin1String("spec_conformance") ||
                        name == QLatin1String("doc_dedup") ||   // ANTS-3660
                        name == QLatin1String("doc_lint") ||    // ANTS-3663
                        // ANTS-2161 — project_settings: project-scoped
                        // layout-config detect + create/update.
                        name == QLatin1String("project_settings") ||
                        // ANTS-2093 — project_query: project-scoped read
                        // (runs a sandboxed snippet over caller_cwd's files).
                        name == QLatin1String("project_query") ||
                        name == QLatin1String("project_layout") ||
                        name == QLatin1String("subsystem") ||
                        // ANTS-1883 — session_orient bundles the
                        // session-start state + project_layout + roadmap
                        // section_index; same workspace family.
                        name == QLatin1String("session_orient"))
                        return QStringLiteral("workspace");
                    if (name == QLatin1String("session_memory") ||
                            // ANTS-4622 — session_message is the cross-SESSION
                            // half of the same question session_memory answers
                            // within one: what survives this session. Bucketed
                            // together so a catalogue reader finds both.
                            name == QLatin1String("session_message") ||
                            // ANTS-6310 — what survives every session, on
                            // every project.
                            name == QLatin1String("know_how") ||
                            name == QLatin1String("workflow_state") ||
                            // ANTS-5299 — run_trace records what a review
                            // run did, for the sessions after it.
                            name == QLatin1String("run_trace"))
                        return QStringLiteral("mcp-state");
                    // ANTS-1735 — model_switch_stats: autonomous
                    // model-switcher effectiveness scorecard.
                    if (name == QLatin1String("model_switch_stats"))
                        return QStringLiteral("model");
                    // ANTS-1309 + ANTS-1308 + ANTS-1963 — spec-aware
                    // tools (spec_log is the write sibling of spec_query).
                    if (name == QLatin1String("spec_query") ||
                        name == QLatin1String("invariant_check") ||
                        name == QLatin1String("spec_log"))
                        return QStringLiteral("spec");
                    // ANTS-1961 / ANTS-1962 — cross-session feedback-file
                    // read + write verbs.
                    if (name == QLatin1String("feedback_query") ||
                        name == QLatin1String("feedback_log"))
                        return QStringLiteral("feedback");
                    // ANTS-1299 — build_status cache.
                    if (name == QLatin1String("build_status") ||
                        // ANTS-3745 — build_target_for: which target owns a
                        // source. A build question answered statically, so it
                        // sits with build_status rather than with the test
                        // runners below.
                        name == QLatin1String("build_target_for"))
                        return QStringLiteral("build");
                    // ANTS-1300 — test_results cache.
                    if (name == QLatin1String("test_results"))
                        return QStringLiteral("test");
                    // ANTS-1302 — focused_test (shares the test bucket).
                    if (name == QLatin1String("focused_test"))
                        return QStringLiteral("test");
                    // ANTS-4398 — mutation_probe (same bucket: it asks
                    // whether the tests measure anything).
                    if (name == QLatin1String("mutation_probe"))
                        return QStringLiteral("test");
                    // ANTS-1303 — symbol queries.
                    if (name == QLatin1String("find_definition") ||
                        name == QLatin1String("trace") ||   // ANTS-5734
                        name == QLatin1String("find_caller"))
                        return QStringLiteral("symbol");
                    // ANTS-1305 — shape matcher.
                    if (name == QLatin1String("similar_code"))
                        return QStringLiteral("pattern");
                    // ANTS-5787 — the readability shortlists.
                    if (name == QLatin1String("function_sizes") ||
                        name == QLatin1String("source_scrapes") ||   // ANTS-5602
                        name == QLatin1String("comment_claims") ||   // ANTS-5786
                        name == QLatin1String("duplicate_blocks"))   // ANTS-5785
                        return QStringLiteral("readability");
                    // ANTS-1306 — task-start context bundler.
                    if (name == QLatin1String("task_priors"))
                        return QStringLiteral("context");
                    // ANTS-1307 — task_type-scoped convention summary.
                    if (name == QLatin1String("project_conventions"))
                        return QStringLiteral("convention");
                    if (name.startsWith(QStringLiteral("get_")) ||
                        name == QLatin1String("tab_list") ||
                        // ANTS-1301 — reads terminal scrollback.
                        name == QLatin1String("recent_errors") ||
                        // ANTS-1312 — reads terminal selection.
                        name == QLatin1String("last_selection"))
                        return QStringLiteral("terminal");
                    if (name == QLatin1String("tool_info") ||
                        name == QLatin1String("token_usage") ||
                        name == QLatin1String("session_cost") ||  // ANTS-5698
                        name == QLatin1String("mcp_trace") ||
                        name == QLatin1String("caller_cwd_info"))
                        return QStringLiteral("meta");
                    return QStringLiteral("other");
                };
                for (int i = 0; i < tools.size(); ++i) {
                    QJsonObject t = tools[i].toObject();
                    const QString name =
                        t.value(QStringLiteral("name")).toString();
                    if (!t.contains(QStringLiteral("version"))) {
                        t[QStringLiteral("version")] = QStringLiteral("1.0");
                    }
                    if (!t.contains(QStringLiteral("typical_token_cost"))) {
                        const auto cost = tokenCostFor(name);
                        t[QStringLiteral("typical_token_cost")] = cost.first;
                        t[QStringLiteral("worst_case_tokens")]  = cost.second;
                    }
                    // ANTS-1518 — prepend `[<kind>] ` to description.
                    // Idempotent: skip if already starts with `[` so a
                    // hot-reload or repeated tools/list call doesn't
                    // double-prefix.
                    // ANTS-1568 — for etag-supporting tools, append a
                    // one-line "Etag tip" memo so a caller scanning the
                    // tools/list block sees WHEN to use the etag_match
                    // shortcut (the parameter doc explains WHAT it
                    // does; the memo explains the workflow). Idempotent
                    // sentinel: check for "Etag tip:" in the existing
                    // description to skip re-append on hot-reload.
                    {
                        QString desc =
                            t.value(QStringLiteral("description")).toString();
                        if (!desc.startsWith(QLatin1Char('['))) {
                            desc = QStringLiteral("[%1] %2")
                                       .arg(kindForName(name), desc);
                        }
                        if (isEtagSupportedTool(name) &&
                            !desc.contains(QStringLiteral("Etag tip:"))) {
                            desc += QStringLiteral(
                                " Etag tip: cache the returned `etag` "
                                "field and pass it back via `etag_match` "
                                "on subsequent calls in the same session "
                                "— saves a full re-emit when the "
                                "underlying file hasn't changed "
                                "(ANTS-1499 \"304 Not Modified\" pattern).");
                        }
                        // One carve-out, about what the verb DOES rather than
                        // who calls it:
                        // indie_review_dispatch POSTs each lane to the
                        // project's configured AI endpoint (default
                        // "llama3"). Its own text says where the request
                        // goes; it does not say the trade-off is review
                        // quality, which is the part a caller choosing a
                        // verb needs. Idempotent sentinel.
                        if (name == QLatin1String("indie_review_dispatch")
                            && !desc.contains(
                                   QStringLiteral("Weaker reviewer:"))) {
                            desc += QStringLiteral(
                                " Weaker reviewer: this reviews on the "
                                "configured local endpoint, NOT on Claude "
                                "subagents — a different and weaker "
                                "reviewer, not a cheaper route to the same "
                                "review. /indie-review keeps its own "
                                "fan-out for that reason; reach for this "
                                "only when the local model is what you "
                                "want (global CLAUDE.md §18).");
                        }
                        t[QStringLiteral("description")] = desc;
                    }
                    // ANTS-1520 — keep the JSON-schema `required[]`
                    // array in sync with the dispatcher's contract.
                    // For every Required-contract tool, ensure
                    // `caller_cwd` is in `inputSchema.required`. Saves
                    // touching ~25 tool-registration blocks individually
                    // and stays in lockstep with callerCwdContractFor
                    // automatically. Idempotent: skips when already
                    // listed (most existing Required tools already
                    // append it explicitly).
                    if (callerCwdContractFor(name) ==
                        CallerCwdContract::Required) {
                        QJsonObject schema =
                            t.value(QStringLiteral("inputSchema"))
                             .toObject();
                        // Guard: only inject when the schema actually
                        // declares `properties.caller_cwd`. A `required`
                        // entry without a matching property would
                        // produce an invalid JSON schema and Zod
                        // validators on the consuming side reject the
                        // whole tools/list response. Most Required-
                        // contract tools declare the property via
                        // `makeCallerCwdReadProp`, some by hand; this
                        // guard covers a registration that skipped it.
                        const QJsonObject props =
                            schema.value(QStringLiteral("properties"))
                                  .toObject();
                        if (props.contains(
                                QStringLiteral("caller_cwd"))) {
                            QJsonArray req =
                                schema.value(QStringLiteral("required"))
                                      .toArray();
                            bool already = false;
                            for (const auto &rv : std::as_const(req)) {
                                if (rv.toString() ==
                                    QStringLiteral("caller_cwd")) {
                                    already = true;
                                    break;
                                }
                            }
                            if (!already) {
                                req.append(QStringLiteral("caller_cwd"));
                                schema[QStringLiteral("required")] = req;
                                t[QStringLiteral("inputSchema")] = schema;
                            }
                        }
                    }
                    // ANTS-4524 — `fields=` is universal, so DECLARE it
                    // universally. The dispatcher projects for every verb now;
                    // an undeclared property is the other half of the same
                    // defect, and it shipped once already (ANTS-4624): a
                    // strict client has no type to marshal to, the argument
                    // arrives as a string, `fv.isArray()` is false and the
                    // narrowing is skipped in silence. Injected here rather
                    // than by ~150 hand-written schema lines, for the reason
                    // the caller_cwd block above gives: one site cannot drift
                    // from the dispatcher. Idempotent — a verb declaring its
                    // own richer `fields` description keeps it, so this adds
                    // the property only where it is missing, with a terse
                    // description because ~150 copies of the full one is real
                    // wire cost on tools/list.
                    {
                        QJsonObject schema =
                            t.value(QStringLiteral("inputSchema")).toObject();
                        if (!schema.isEmpty()) {
                            QJsonObject props =
                                schema.value(QStringLiteral("properties"))
                                      .toObject();
                            if (!props.contains(QStringLiteral("fields"))) {
                                QJsonObject p;
                                p[QStringLiteral("type")] =
                                    QStringLiteral("array");
                                QJsonObject items;
                                items[QStringLiteral("type")] =
                                    QStringLiteral("string");
                                p[QStringLiteral("items")] = items;
                                p[QStringLiteral("description")] =
                                    QStringLiteral(
                                        "Optional. Return only these top-level "
                                        "response fields; names the envelope "
                                        "does not carry are listed in "
                                        "`fields_unmatched`, alongside "
                                        "`fields_available` — the keys it does "
                                        "carry, so a misspelling is visible "
                                        "rather than inferred (ANTS-4930). "
                                        "Honoured by every verb (ANTS-4524).");
                                props[QStringLiteral("fields")] = p;
                                schema[QStringLiteral("properties")] = props;
                                t[QStringLiteral("inputSchema")] = schema;
                            }
                        }
                    }
                    // ANTS-2158 — exempt the highest-frequency verbs from
                    // Claude Code's MCP tool-search DEFERRAL so they are
                    // callable without a ToolSearch round-trip (the
                    // "deferred-schema tax" that nudged sessions back to raw
                    // grep/Read/Edit). Honoured by Claude Code v2.1.121+ via
                    // the tool's `_meta`; older clients ignore the field
                    // (graceful). The set is small on purpose — each
                    // always-loaded tool costs context in every session, so
                    // only the verbs that most directly replace the
                    // always-loaded Bash grep / Read. The roadmap and
                    // changelog writers are not in it: their schemas are
                    // large and most sessions call them rarely.
                    // See docs/standards/mcp-behavioural-notes.md.
                    static const QSet<QString> kEagerVerbs = {
                        QStringLiteral("workspace_search"),
                        QStringLiteral("find_definition"),
                        // ANTS-5734 § 2.8 — listed only where registered.
                        QStringLiteral("trace"),
                        QStringLiteral("file_outline"),
                        QStringLiteral("read_region"),
                    };
                    if (kEagerVerbs.contains(
                            t.value(QStringLiteral("name")).toString())) {
                        QJsonObject meta =
                            t.value(QStringLiteral("_meta")).toObject();
                        meta[QStringLiteral("anthropic/alwaysLoad")] = true;
                        t[QStringLiteral("_meta")] = meta;
                    }
                    tools.replace(i, t);
                }

                // ANTS-1399-INV-2 — snapshot for tool_info to read from
                // without re-running the array build. Descriptors are
                // built from compile-time literals so the snapshot
                // never goes stale in practice. Snapshot the FULL
                // tools array before any lite-shape transform so a
                // tools/list-lite client can still drill in via
                // tool_info(name) for the full schema.
                m_lastToolsList = tools;

                // ANTS-2175 — cache each verb's declared inputSchema property
                // names so the tools/call dispatcher can flag args a verb
                // doesn't recognise (a typo'd / stale param, silently dropped
                // today) via an `ignored_args` advisory. Rebuilt in lockstep
                // with the snapshot above so it can never go stale; the schema
                // is compile-time-literal so this fires ~once per session.
                // Derived from the FULL array (pre-detail-strip) — the strip
                // only touches `description`, not `inputSchema`.
                m_toolParamKeys.clear();
                m_toolParamSchemas.clear();
                for (const auto &v : std::as_const(tools)) {
                    const QJsonObject t = v.toObject();
                    const QString name =
                        t.value(QStringLiteral("name")).toString();
                    if (name.isEmpty()) continue;
                    const QJsonObject props =
                        t.value(QStringLiteral("inputSchema")).toObject()
                         .value(QStringLiteral("properties")).toObject();
                    QSet<QString> keys;
                    for (auto pit = props.constBegin();
                         pit != props.constEnd(); ++pit)
                        keys.insert(pit.key());
                    m_toolParamKeys.insert(name, keys);
                    m_toolParamSchemas.insert(name, props);
                }

                // ANTS-5506 — doc_lint's doc_facts reads this map from a
                // dispatch worker, so it gets a COPY published under a mutex
                // rather than m_toolParamKeys itself (GUI-thread-only,
                // ANTS-5072). The set is what a call may pass without an
                // ignored_args advisory: schema properties, the universal
                // dispatch args, and the conditional ones this verb honours.
                {
                    QHash<QString, QSet<QString>> published;
                    for (auto kit = m_toolParamKeys.cbegin();
                         kit != m_toolParamKeys.cend(); ++kit) {
                        QSet<QString> accepted = kit.value();
                        accepted.insert(QStringLiteral("caller_cwd"));
                        accepted.insert(QStringLiteral("encoding"));
                        accepted.insert(QStringLiteral("fields"));
                        accepted.unite(honouredDispatchArgs(kit.key()));
                        published.insert(kit.key(), accepted);
                    }
                    QMutexLocker lock(&m_verbArgsMutex);
                    m_verbArgsPublished = std::move(published);
                }

                // ANTS-2079 — strip per-op `detail` from the wire payload;
                // the snapshot above retains it so tool_info can serve it on
                // demand. QJsonArray is copy-on-write: mutating `tools` here
                // detaches it, leaving m_lastToolsList intact. A one-line
                // pointer is appended to `description` so a session reading
                // tools/list knows where the full prose lives. Runs before
                // both the lite branch and the full-shape send so neither
                // wire shape carries `detail`.
                for (int i = 0; i < tools.size(); ++i) {
                    QJsonObject t = tools.at(i).toObject();
                    if (t.contains(QStringLiteral("detail"))) {
                        t.remove(QStringLiteral("detail"));
                        t[QStringLiteral("description")] =
                            t.value(QStringLiteral("description")).toString()
                            + QStringLiteral(
                                  " Full per-op detail via tool_info "
                                  "{name:\"%1\"}.")
                                  .arg(t.value(QStringLiteral("name"))
                                           .toString());
                        tools.replace(i, t);
                    }
                }

                // ANTS-5628 — an always-loaded verb's schema is paid for by
                // every session at start, so on the wire each of its argument
                // descriptions becomes a one-line summary. The snapshot above
                // keeps the full text, and tool_info {name} serves it, which
                // the description's pointer already names. Keyed by
                // "verb/arg" first, then by "arg" for the arguments those
                // verbs share. A summary is used only where it is shorter;
                // an argument with no entry keeps its own text.
                {
                    static const QHash<QString, QString> kArgSummary = {
                        {QStringLiteral("caller_cwd"), QStringLiteral("Your $PWD (required); picks the project.")},
                        {QStringLiteral("compact"), QStringLiteral("Drop null, false and empty fields from the reply.")},
                        {QStringLiteral("encoding"), QStringLiteral("\"tabular\" packs object arrays as {__cols__, __rows__}; default \"json\".")},
                        {QStringLiteral("etag_match"), QStringLiteral("Etag from an earlier call with the same arguments; an unchanged result returns {unchanged:true}.")},
                        {QStringLiteral("fields"), QStringLiteral("Return only these top-level reply fields.")},
                        {QStringLiteral("raw"), QStringLiteral("Return the file text verbatim; use it when you will Edit from the output.")},
                        {QStringLiteral("workspace_search/context"), QStringLiteral("Lines of context around each match (0-10).")},
                        {QStringLiteral("workspace_search/count_only"), QStringLiteral("Return only the match and file counts.")},
                        {QStringLiteral("workspace_search/dedup"), QStringLiteral("Fold identical lines into one match with also_at (default true).")},
                        {QStringLiteral("workspace_search/enclosing_symbol"), QStringLiteral("Name the function each match is inside.")},
                        {QStringLiteral("workspace_search/exclude_glob"), QStringLiteral("Glob or globs to leave out, written positively (\"docs/**\").")},
                        {QStringLiteral("workspace_search/files_only"), QStringLiteral("Return only the matching files, with hit counts.")},
                        {QStringLiteral("workspace_search/glob"), QStringLiteral("One inclusion glob, e.g. \"*.cpp\" or \"docs/{a,b}.md\".")},
                        {QStringLiteral("workspace_search/headline_only"), QStringLiteral("Rows as {file, line, headline}, without context.")},
                        {QStringLiteral("workspace_search/lane"), QStringLiteral("Subdirectory to search, e.g. \"src\".")},
                        {QStringLiteral("workspace_search/match_wrapped"), QStringLiteral("Match a literal quotation across hard line wraps.")},
                        {QStringLiteral("workspace_search/matches_only"), QStringLiteral("Return the distinct matched strings, with counts.")},
                        {QStringLiteral("workspace_search/max_bytes"), QStringLiteral("Reply size cap in bytes (default 512 KiB).")},
                        {QStringLiteral("workspace_search/multiline"), QStringLiteral("Let a regex cross line breaks; write the break as \\n or \\s+.")},
                        {QStringLiteral("workspace_search/max_match_bytes"), QStringLiteral("Clip each match's text to N bytes (default 512; 0 = no clip).")},
                        {QStringLiteral("workspace_search/offset"), QStringLiteral("Skip the first N matches; pass back next_offset to page.")},
                        {QStringLiteral("workspace_search/query"), QStringLiteral("Alias for pattern.")},
                        {QStringLiteral("workspace_search/respect_gitignore"), QStringLiteral("false also searches ignored files.")},
                        {QStringLiteral("workspace_search/timeout_sec"), QStringLiteral("ripgrep time budget in seconds (1-30, default 5).")},
                        {QStringLiteral("file_outline/etags"), QStringLiteral("{path: etag} for the paths form; an unchanged file returns a stub.")},
                        {QStringLiteral("file_outline/filter"), QStringLiteral("Keep only symbols whose name contains this text.")},
                        {QStringLiteral("file_outline/max_bytes"), QStringLiteral("Reply size cap in bytes (default 512 KiB).")},
                        {QStringLiteral("file_outline/max_heading_level"), QStringLiteral("Markdown: drop headings deeper than this level.")},
                        {QStringLiteral("file_outline/max_symbols"), QStringLiteral("Cap on symbols[] (default 200).")},
                        {QStringLiteral("file_outline/paths"), QStringLiteral("Several paths in one call; returns files[].")},
                        {QStringLiteral("file_outline/sizes"), QStringLiteral("Add bytes and lines to each symbol.")},
                        {QStringLiteral("read_region/call_sequence"), QStringLiteral("Also return the region's calls in order, and the accessors it uses.")},
                        {QStringLiteral("read_region/max_line_bytes"), QStringLiteral("Clip each line to N bytes.")},
                        {QStringLiteral("read_region/section"), QStringLiteral("Markdown heading text or slug; returns that section.")},
                        {QStringLiteral("find_definition/include_body"), QStringLiteral("Return each definition's body inline.")},
                        {QStringLiteral("workspace_search/include_hidden"), QStringLiteral("Also search dotfile paths (never .git/).")},
                        {QStringLiteral("file_outline/path"), QStringLiteral("File path, repo-relative or absolute, under the project.")},
                        {QStringLiteral("read_region/start_line"), QStringLiteral("1-based first line.")},
                        {QStringLiteral("read_region/end_line"), QStringLiteral("1-based last line, inclusive (default start_line).")},
                        {QStringLiteral("read_region/symbol"), QStringLiteral("Return this symbol's body.")},
                        {QStringLiteral("read_region/tail_lines"), QStringLiteral("With section: only its last N lines.")},
                        {QStringLiteral("read_region/max_bytes"), QStringLiteral("Cap on lines[] bytes (default 512 KiB); keeps the head.")},
                        {QStringLiteral("find_definition/lang"), QStringLiteral("One language family; auto (default) scans all.")},
                        {QStringLiteral("find_definition/max_results"), QStringLiteral("Cap on definitions[] (default 50).")},
                    };
                    static const QSet<QString> kSharedArgs = {
                        QStringLiteral("fields"), QStringLiteral("compact"),
                        QStringLiteral("encoding"), QStringLiteral("etag_match"),
                    };
                    // ANTS-5694 — the always-loaded verbs' own descriptions: the
                    // purpose, the one usage trap, and (on the two etag
                    // verbs) a one-line Etag tip. The kind prefix and the
                    // tool_info pointer are added below, as on every verb.
                    static const QHash<QString, QString> kEagerDescription = {
                        {QStringLiteral("workspace_search"), QStringLiteral("Search a project for a literal string, or a regex with regex:true; use instead of Bash grep -r. ONE pattern, not AND-ed words. caller_cwd picks the tree (any directory). Returns matches[{file, line, text}]. Holding a symbol name? find_caller or trace is cheaper.")},
                        {QStringLiteral("find_definition"), QStringLiteral("Find where a symbol is defined (C++, Python, Lua, Shell, GLSL) instead of grep + Read. Returns definitions[{file, line, signature, kind}]; kind is definition, declaration or local.")},
                        {QStringLiteral("file_outline"), QStringLiteral("Outline a file (header comment + symbols {line, kind, name, signature}) instead of a full Read; paths:[...] outlines several. caller_cwd required, or \"~global\". Does NOT satisfy Edit's read-precondition. Etag tip: pass a returned etag back as etag_match.")},
                        {QStringLiteral("read_region"), QStringLiteral("Return an exact slice of a file instead of a Read: a line range, a symbol's body, or a markdown section; exactly one. Several: read_regions. caller_cwd required, or \"~global\" for ~/.claude/. Does NOT satisfy Edit's read-precondition. Etag tip: pass a returned etag back as etag_match.")},
                    };
                    // ANTS-5271 — the five largest schemas are deferred, but a
                    // session that loads one carries it to the end, and most
                    // calls use two or three of its arguments. Same treatment,
                    // nested item fields included ("verb/parent.child" first).
                    // ANTS-4931 — the six verbs whose argument prose was
                    // longest after those five (measured 2026-11-04).
                    static const QSet<QString> kLeanVerbs = {
                        QStringLiteral("roadmap_log"),
                        QStringLiteral("roadmap_query"),
                        QStringLiteral("feedback_log"),
                        QStringLiteral("changelog_log"),
                        QStringLiteral("roadmap_migrate"),
                        QStringLiteral("audit_run"),
                        QStringLiteral("apply_edits"),
                        QStringLiteral("doc_citations"),
                        QStringLiteral("doc_lint"),
                        QStringLiteral("spec_log"),
                        QStringLiteral("invariant_check"),
                    };
                    static const QHash<QString, QString> kLeanSummary = {
                        {QStringLiteral("dry_run"), QStringLiteral("Preview the result without writing (default false).")},
                        {QStringLiteral("roadmap_query/body_from_end"), QStringLiteral("id/ids fetch: keep the END of an over-cap body; max_body_bytes then sizes the tail.")},
                        {QStringLiteral("roadmap_query/body_match"), QStringLiteral("Keep only body lines containing this text (case-insensitive); turns bodies on.")},
                        {QStringLiteral("roadmap_query/bullet_fields"), QStringLiteral("Keep only these keys on each bullet. Not with headline_only, section_index, bundles or report.")},
                        {QStringLiteral("roadmap_query/check_sync"), QStringLiteral("Also report whether ROADMAP.md matches the store (file_in_sync, drift_*). Not cached; ask once.")},
                        {QStringLiteral("roadmap_query/filter"), QStringLiteral("Alias for status.")},
                        {QStringLiteral("roadmap_query/id"), QStringLiteral("Fetch one bullet by id (case-sensitive), body included.")},
                        {QStringLiteral("roadmap_query/ids"), QStringLiteral("Fetch up to 100 bullets by id, in document order, each with input_index; misses in missing_ids.")},
                        {QStringLiteral("roadmap_query/include_body"), QStringLiteral("Add each bullet's body (2000-char cap on lists). id/ids fetches include it by default.")},
                        {QStringLiteral("roadmap_query/include_narrator_bullets"), QStringLiteral("Keep id-less section-summary bullets (default false).")},
                        {QStringLiteral("roadmap_query/include_section_etags"), QStringLiteral("section_index: add a section_etag per section, for section_etag_match.")},
                        {QStringLiteral("roadmap_query/include_section_headers"), QStringLiteral("Keep section-rollup bullets that carry no id or headline (default false).")},
                        {QStringLiteral("roadmap_query/kind"), QStringLiteral("Keep only bullets of this kind; an unknown kind refuses bad_kind.")},
                        {QStringLiteral("roadmap_query/limit"), QStringLiteral("Cap on bullets[] (1-500). Omitted, a large reply downshifts to headline rows.")},
                        {QStringLiteral("roadmap_query/max_body_bytes"), QStringLiteral("Body cap: one id up to 1 MiB, ids up to 16 KiB each; on lists it may only lower the 2000 cap.")},
                        {QStringLiteral("roadmap_query/max_results"), QStringLiteral("Alias for limit.")},
                        {QStringLiteral("roadmap_query/mode"), QStringLiteral("bullets (default), section_index (slugs and counts), headline_only, bundles, report, sources, or ledger.")},
                        {QStringLiteral("roadmap_query/offset"), QStringLiteral("0-based start into the filtered bullets; use with limit to page.")},
                        {QStringLiteral("roadmap_query/q"), QStringLiteral("Alias for query.")},
                        {QStringLiteral("roadmap_query/query"), QStringLiteral("Case-insensitive substring filter on headline, body or id. On section_index it filters section names.")},
                        {QStringLiteral("roadmap_query/range"), QStringLiteral("mode:ledger only: git range checked for commits naming no roadmap id (default newest tag..HEAD).")},
                        {QStringLiteral("roadmap_query/regex"), QStringLiteral("Read query as a case-insensitive regular expression.")},
                        {QStringLiteral("roadmap_query/scope"), QStringLiteral("mode:report only: project (default) or all registered projects.")},
                        {QStringLiteral("roadmap_query/section"), QStringLiteral("Slug of a ## or ### heading; returns its bullets and its subsections' bullets.")},
                        {QStringLiteral("roadmap_query/section_etag_match"), QStringLiteral("section_etag from an earlier section= call; an unchanged section returns {unchanged:true}.")},
                        {QStringLiteral("roadmap_query/shipped_since"), QStringLiteral("YYYY-MM-DD: list items the store says shipped on or after this date. Store-backed projects only.")},
                        {QStringLiteral("roadmap_query/shipped_until"), QStringLiteral("YYYY-MM-DD, exclusive upper bound for shipped_since (default tomorrow).")},
                        {QStringLiteral("roadmap_query/since"), QStringLiteral("mode:report only: YYYY-MM-DD start of one custom period.")},
                        {QStringLiteral("roadmap_query/slugs_only"), QStringLiteral("section_index only: return a flat slugs array.")},
                        {QStringLiteral("roadmap_query/source"), QStringLiteral("Keep bullets whose Source: starts with this prefix (string or array, case-insensitive).")},
                        {QStringLiteral("roadmap_query/source_group"), QStringLiteral("mode:sources: stem (default; drops the date onward) or exact.")},
                        {QStringLiteral("roadmap_query/stale_days"), QStringLiteral("mode:ledger only: an in-progress item untouched this many days is stale (default 14).")},
                        {QStringLiteral("roadmap_query/status"), QStringLiteral("active (planned + in-progress), shipped, all, one lifecycle name or emoji, or an array of them.")},
                        {QStringLiteral("roadmap_query/version"), QStringLiteral("A release such as \"0.10.0\"; answers as section= does for that release's section.")},
                        {QStringLiteral("roadmap_query/whole_word"), QStringLiteral("Match query on word boundaries, so a short acronym stops matching inside words.")},
                        {QStringLiteral("roadmap_query/with_titles"), QStringLiteral("With slugs_only, also return slug_titles [{slug, headline}].")},
                        {QStringLiteral("roadmap_log/accept_ambiguous_rematch"), QStringLiteral("convert: proceed although some bullets would be paired with stored items by order.")},
                        {QStringLiteral("roadmap_log/accept_text_loss"), QStringLiteral("convert: proceed although the rewrite drops text the file holds.")},
                        {QStringLiteral("roadmap_log/after_section"), QStringLiteral("create_section / move_section: the slug the new or moved section goes after.")},
                        {QStringLiteral("roadmap_log/amendments"), QStringLiteral("amend_field_batch: [{id, field, value}] in one commit; refused entries land in skipped[].")},
                        {QStringLiteral("roadmap_log/before_id"), QStringLiteral("amend_field field:section: file the moved items directly before this item.")},
                        {QStringLiteral("roadmap_log/bullets"), QStringLiteral("append_batch: bullets with append's fields; {{id:N}} in text cites bullet N's new id.")},
                        {QStringLiteral("roadmap_log/bullets.id_hint"), QStringLiteral("Explicit id number; honoured on the first bullet only (counter strategy).")},
                        {QStringLiteral("roadmap_log/bullets.pass"), QStringLiteral("This bullet's pass designator on a pass-headings roadmap; the call-level pass is the fallback.")},
                        {QStringLiteral("roadmap_log/bullets.section"), QStringLiteral("This bullet's section slug. The call-level section is the fallback; omit it when every bullet names one.")},
                        {QStringLiteral("roadmap_log/bullets.stable_id"), QStringLiteral("This bullet's id when id_strategy is stable_prefix; unique within the batch.")},
                        {QStringLiteral("roadmap_log/bullets.status"), QStringLiteral("planned, in-progress, shipped, considered or dropped; the call-level status is the fallback.")},
                        {QStringLiteral("roadmap_log/cells"), QStringLiteral("bundle_row: one string per table column.")},
                        {QStringLiteral("roadmap_log/clear_placeholder_source"), QStringLiteral("repair_trailers: also blank an undated placeholder source of \"planned\".")},
                        {QStringLiteral("roadmap_log/dry_run"), QStringLiteral("Preview without writing. The store change runs and is rolled back, so refusals still show.")},
                        {QStringLiteral("roadmap_log/element_position"), QStringLiteral("amend/delete/promote_element: the element's position as list_elements reports it.")},
                        {QStringLiteral("roadmap_log/evidence"), QStringLiteral("File paths (screenshots, logs) evidencing the item. Paths only: the list is comma-split.")},
                        {QStringLiteral("roadmap_log/field"), QStringLiteral("amend_field: layman, kind, source, lanes or evidence; or section, to move the item.")},
                        {QStringLiteral("roadmap_log/force"), QStringLiteral("append: file a bullet whose headline matches an existing item.")},
                        {QStringLiteral("roadmap_log/header"), QStringLiteral("bundle_row: column names; creates the table when the section has none.")},
                        {QStringLiteral("roadmap_log/id_hint"), QStringLiteral("append: explicit id number; must exceed the counter.")},
                        {QStringLiteral("roadmap_log/id_prefix"), QStringLiteral("append: id prefix (e.g. \"DOOM\"); default sniffed from existing ids, else the folder name.")},
                        {QStringLiteral("roadmap_log/id_strategy"), QStringLiteral("counter (default) or stable_prefix, which needs stable_id.")},
                        {QStringLiteral("roadmap_log/intro_body"), QStringLiteral("create_section: intro prose for the new section.")},
                        {QStringLiteral("roadmap_log/legend"), QStringLiteral("set_legend: {status: wording} for the status legend; an empty string removes one.")},
                        {QStringLiteral("roadmap_log/locators"), QStringLiteral("Batch ops: items, each {id|anchor|headline|line_range} plus note, to_status or old_text/new_text.")},
                        {QStringLiteral("roadmap_log/locators.note"), QStringLiteral("Note appended to this item's body; required on annotate_batch.")},
                        {QStringLiteral("roadmap_log/locators.old_text"), QStringLiteral("amend_batch: the exact text to replace in this item's body.")},
                        {QStringLiteral("roadmap_log/locators.to_status"), QStringLiteral("flip_batch: this item's target status; the call-level to_status is the fallback.")},
                        {QStringLiteral("roadmap_log/max_planned"), QStringLiteral("convert: cap on ids.planned[] rows (default 200); 0 keeps only the counts.")},
                        {QStringLiteral("roadmap_log/new_text"), QStringLiteral("amend_*, set_body, set_intro, set_preamble: the replacement text; empty deletes the match.")},
                        {QStringLiteral("roadmap_log/note"), QStringLiteral("Prose appended to the item's body; required on annotate, optional on flip.")},
                        {QStringLiteral("roadmap_log/old_text"), QStringLiteral("amend_body / amend_headline: exact text; must occur once; matches across wraps. replace_text: every exact occurrence.")},
                        {QStringLiteral("roadmap_log/op"), QStringLiteral("Default append. Each op's arguments, refusals and behaviour: tool_info {name:\"roadmap_log\"}.")},
                        {QStringLiteral("roadmap_log/plan_id"), QStringLiteral("A dry run's plan_id: applies that request without resending it. Used once; lasts 15 minutes.")},
                        {QStringLiteral("roadmap_log/pass"),QStringLiteral("Pass designator (e.g. \"43.5\") for append on a pass-headings roadmap; required there.")},
                        {QStringLiteral("roadmap_log/position"), QStringLiteral("bundle_row: end (default), or sorted by sort_col.")},
                        {QStringLiteral("roadmap_log/return"), QStringLiteral("headline_only adds post_bullets to append/flip replies; full echoes the whole rendered bullet.")},
                        {QStringLiteral("roadmap_log/section"), QStringLiteral("Section slug; an unknown slug refuses with candidates[]. replace_text: limits it to this section and its sub-sections.")},
                        {QStringLiteral("roadmap_log/stable_id"), QStringLiteral("The full id when id_strategy is stable_prefix (e.g. \"Ts20-SP6\").")},
                        {QStringLiteral("roadmap_log/status"), QStringLiteral("planned, in-progress, shipped, considered or dropped. replace_text: only items with this status.")},
                        {QStringLiteral("roadmap_log/strip_runs"), QStringLiteral("repair_trailers: also remove a redundant legacy trailer run that ends a body.")},
                        {QStringLiteral("roadmap_log/targets"), QStringLiteral("link / unlink: the target ids.")},
                        {QStringLiteral("roadmap_log/type"), QStringLiteral("link / unlink: splits-from, blocked-by, duplicate-of or supersedes.")},
                        {QStringLiteral("roadmap_log/value"), QStringLiteral("amend_field: the new value; lanes and evidence take an array; empty clears layman.")},
                        {QStringLiteral("feedback_log/assignments"), QStringLiteral("assign_id_batch: assign_id argument objects applied in one write; failures land in skipped[].")},
                        {QStringLiteral("feedback_log/awaiting"), QStringLiteral("assign_id: a question for the reporter; the finding stays untriaged. One of ids/closure/awaiting.")},
                        {QStringLiteral("feedback_log/confirm"), QStringLiteral("reimport: replace the store's text with the hand-edited file's.")},
                        {QStringLiteral("feedback_log/heading"), QStringLiteral("assign_id: the finding's verbatim ### heading line.")},
                        {QStringLiteral("feedback_log/ids"), QStringLiteral("assign_id: the ANTS-NNNN ids to assign. One of ids/closure/awaiting.")},
                        {QStringLiteral("feedback_log/note"), QStringLiteral("Prose under the heading; on assign_id, a single Note line.")},
                        {QStringLiteral("feedback_log/op"), QStringLiteral("Required. Contributors use append_finding; the rest are maintainer ops. Detail: tool_info.")},
                        {QStringLiteral("feedback_log/path"), QStringLiteral("The feedback file; omitted, derived from caller_cwd's folder name at the shared root.")},
                        {QStringLiteral("feedback_log/title"), QStringLiteral("set_title: the project name, replacing the text after the H1's last em dash.")},
                        {QStringLiteral("roadmap_migrate/accept_deletions"), QStringLiteral("Delete items missing from the file; a store-served project keeps them by default (absent_kept).")},
                        {QStringLiteral("roadmap_migrate/backup"), QStringLiteral("Snapshot the store first (default true); a failed snapshot refuses.")},
                        {QStringLiteral("roadmap_migrate/backup_to"), QStringLiteral("Snapshot path; default pre-migrate.sqlite in the snapshot folder. Overwritten each run.")},
                        {QStringLiteral("roadmap_migrate/confirm"), QStringLiteral("deregister / reroot: confirm while the (old) project root still exists.")},
                        {QStringLiteral("roadmap_migrate/dry_run"), QStringLiteral("Plan and report counts, then roll back.")},
                        {QStringLiteral("roadmap_migrate/export_slug"), QStringLiteral("The project's export key, [a-z0-9-]; defaults to the slugified folder name.")},
                        {QStringLiteral("roadmap_migrate/max_notes"), QStringLiteral("Cap on notes[] rows (default 200, clamped to 1-2000).")},
                        {QStringLiteral("roadmap_migrate/only_ids"), QStringLiteral("Report only these ids in updated_items[]; the migration still runs whole.")},
                        {QStringLiteral("roadmap_migrate/op"), QStringLiteral("migrate (default), deregister (delete the project), init (new roadmap) or reroot (moved root).")},
                        {QStringLiteral("changelog_log/body"), QStringLiteral("Prose under the bullet, stored as sent, so wrap it yourself. On add_from_roadmap it replaces the Layman line.")},
                        {QStringLiteral("changelog_log/bullets"), QStringLiteral("add_subsection: bullets under the prose, each {summary, body?, id?}.")},
                        {QStringLiteral("changelog_log/date"), QStringLiteral("add_subsection / release: YYYY-MM-DD, default today.")},
                        {QStringLiteral("changelog_log/dated"), QStringLiteral("release: close under ## <date>, for a project with no versions.")},
                        {QStringLiteral("changelog_log/entries"), QStringLiteral("add_batch: entries, each {summary + category|kind} or {id} alone; written in input order.")},
                        {QStringLiteral("changelog_log/headline"), QStringLiteral("add_subsection: the dated topic headline.")},
                        {QStringLiteral("changelog_log/max_body_bytes"), QStringLiteral("release: cap on the echoed released_body (default 16384; 0 = no cap).")},
                        {QStringLiteral("changelog_log/op"), QStringLiteral("Default add. Each op's arguments and refusals: tool_info {name:\"changelog_log\"}.")},
                        {QStringLiteral("changelog_log/path"), QStringLiteral("A second, existing changelog (project-relative); omit for the root CHANGELOG.md.")},
                        {QStringLiteral("changelog_log/summary"), QStringLiteral("Bold one-line summary; on add_from_roadmap it replaces the cited headline.")},
                        {QStringLiteral("changelog_log/version"), QStringLiteral("release: the version, without brackets.")},
                        {QStringLiteral("apply_edits/dry_run"), QStringLiteral("Preview without writing; the reply says would_write / would_apply instead of files_written (default false).")},
                        {QStringLiteral("apply_edits/edits"), QStringLiteral("{path?, new} plus {old, replace_all?} or {start_line, end_line, expect_first_line, expect_last_line}; old_string/new_string accepted.")},
                        {QStringLiteral("apply_edits/edits.expect_count"), QStringLiteral("old form only: how many times old must match; another count skips the edit as count_mismatch. Refused with a range.")},
                        {QStringLiteral("apply_edits/edits.expect_first_line"), QStringLiteral("Verbatim text of line start_line; required on a range edit. A mismatch skips it as range_mismatch.")},
                        {QStringLiteral("apply_edits/edits.expect_last_line"), QStringLiteral("Verbatim text of line end_line; required on a range edit, checked with expect_first_line.")},
                        {QStringLiteral("apply_edits/edits.old_text"), QStringLiteral("Alias for old (roadmap_log amend_body's spelling). Sending two spellings refuses bad_op_combo.")},
                        {QStringLiteral("apply_edits/match_wrapped"), QStringLiteral("Let `old` match across different line wraps; a wrap-caused not_found names this flag in its hint (default false).")},
                        {QStringLiteral("apply_edits/path"), QStringLiteral("Default path for every edit that omits one; a per-edit path wins.")},
                        {QStringLiteral("doc_lint/checks"), QStringLiteral("Checkers to run (default all): doc_integrity, doc_citations, doc_dedup, doc_symbols, spec_lint, doc_facts. Unknown refuses.")},
                        {QStringLiteral("doc_lint/dry_run"), QStringLiteral("With fix:true, compute the repair and write nothing; refuses without fix.")},
                        {QStringLiteral("doc_lint/fix"), QStringLiteral("Repair toc_gap findings in place (default false; all else is report-only). Adds fixed[], files_written, fix_errors[].")},
                        {QStringLiteral("doc_lint/max_findings"), QStringLiteral("Cap on findings[] (default 500, 1-5000); counts still cover the whole run.")},
                        {QStringLiteral("doc_citations/quotes"), QStringLiteral("Also check double-quoted spans against the document a nearby backticked path names; adds quotes[], quote_counts (default off).")},
                        {QStringLiteral("spec_log/cells"), QStringLiteral("append_loop on a table log: one string per column, in header order; a wrong count refuses column_mismatch.")},
                        {QStringLiteral("spec_log/dry_run"), QStringLiteral("Return the landing line and would-be file size (bytes) without writing (default false).")},
                        {QStringLiteral("spec_log/preserve_body"), QStringLiteral("set_status only: keep the Status field's continuation lines, rewrite only its first line (default false).")},
                        {QStringLiteral("spec_log/status"), QStringLiteral("set_status: the new value, one line (continuations replaced unless preserve_body). append_status: text added at the field's end.")},
                        {QStringLiteral("invariant_check/files"), QStringLiteral("Project-relative paths to find citing specs for; pass src/foo.cpp, not foo.cpp. Read basename_matches too.")},
                        {QStringLiteral("invariant_check/mode"), QStringLiteral("summary (default: counts, no bodies) or full (adds invariant bodies; can exceed the reply cap).")},
                        {QStringLiteral("invariant_check/paths"), QStringLiteral("Alias for files; sending both refuses bad_op_combo.")},
                        // Its own wording, kept: the shared caller_cwd summary is
                        // shorter and would drop the mismatch refusal.
                        {QStringLiteral("audit_run/caller_cwd"), QStringLiteral("Your $PWD. Mutating verbs refuse on mismatch with the focused tab's cwd (ANTS-1372).")},
                        {QStringLiteral("audit_run/async"), QStringLiteral("Return a job handle at once and poll audit_poll {job_id}; for scope:\"full\" or slow tools (default false).")},
                        {QStringLiteral("audit_run/cap_per_tool_seconds"), QStringLiteral("Per-tool wall-clock cap, 5-300 s (default 30); above ~60 s use async:true. Out of range refuses bad_args.")},
                        {QStringLiteral("audit_run/checks"), QStringLiteral("Check ids for clang-tidy (e.g. [\"bugprone-integer-division\"]); other tools refuse bad_args.")},
                        {QStringLiteral("audit_run/exclude_paths"), QStringLiteral("Path prefixes to leave out (vendored code); exclude_paths_ignored_by names tools that cannot honour it.")},
                        {QStringLiteral("audit_run/paths"), QStringLiteral("Paths to narrow file-scoped tools to; gitleaks and trivy then skip.")},
                        {QStringLiteral("audit_run/scope"), QStringLiteral("auto (default), full (whole tracked src/, deterministic), files, since-last-run (adds delta), since-tag:<tag>, branch-diff.")},
                        {QStringLiteral("audit_run/suppressions"), QStringLiteral("auto (default: learned-FP ledger + allowlist), none, or path:<file>.")},
                        {QStringLiteral("audit_run/tools"), QStringLiteral("Subset of cppcheck, clazy, clang-tidy, ruff, bandit, semgrep, gitleaks, trivy, shellcheck, mypy; default all but mypy.")},
                    };
                    // ANTS-5725 — an always-loaded verb's arguments used in
                    // under 5% of its calls stay off the wire (user decision,
                    // 2026-10-16). They are still accepted: m_toolParamKeys
                    // and tool_info read the saved full list, and Claude Code
                    // passes an argument the wire schema leaves out, as text
                    // (ANTS-6325: mcp::coerceArgTypes restores its type). Measured 2026-10-22 with
                    // tools/mcp-arg-usage-survey.py; re-run it before adding
                    // to or taking from a list. find_definition hides none.
                    static const QHash<QString, QStringList> kWireHidden = {
                        {QStringLiteral("workspace_search"),
                         {QStringLiteral("case"), QStringLiteral("count_only"),
                          QStringLiteral("dedup"), QStringLiteral("encoding"),
                          QStringLiteral("fields"), QStringLiteral("include_hidden"),
                          QStringLiteral("match_wrapped"), QStringLiteral("matches_only"),
                          QStringLiteral("max_bytes"), QStringLiteral("multiline"),
                          QStringLiteral("offset"),
                          QStringLiteral("query"), QStringLiteral("raw"),
                          QStringLiteral("respect_gitignore"), QStringLiteral("timeout_sec")}},
                        {QStringLiteral("file_outline"),
                         {QStringLiteral("mode"), QStringLiteral("etag_match"),
                          QStringLiteral("etags"), QStringLiteral("max_bytes"),
                          QStringLiteral("raw")}},
                        {QStringLiteral("read_region"),
                         {QStringLiteral("call_sequence"), QStringLiteral("compact"),
                          QStringLiteral("etag_match"), QStringLiteral("fields"),
                          QStringLiteral("max_bytes"), QStringLiteral("raw"),
                          QStringLiteral("tail_lines")}},
                    };
                    // The first key with an entry wins: "verb/parent.child"
                    // (nested only), "verb/arg", then the shared "arg".
                    const auto summaryFor = [](const QStringList &keys) {
                        for (const QString &key : keys) {
                            QString s = kArgSummary.value(key);
                            if (s.isEmpty()) s = kLeanSummary.value(key);
                            if (!s.isEmpty()) return s;
                        }
                        return QString();
                    };
                    const auto applySummary = [](QJsonObject &p, const QString &s) {
                        if (!s.isEmpty()
                            && s.size() < p.value(QStringLiteral("description"))
                                              .toString().size())
                            p[QStringLiteral("description")] = s;
                    };
                    const auto shorten = [&](QJsonObject &props, const QString &name) {
                        const QString verb = name + QLatin1Char('/');
                        for (auto pit = props.begin(); pit != props.end(); ++pit) {
                            QJsonObject p = pit.value().toObject();
                            applySummary(p, summaryFor({verb + pit.key(), pit.key()}));
                            QJsonObject items =
                                p.value(QStringLiteral("items")).toObject();
                            QJsonObject inner =
                                items.value(QStringLiteral("properties")).toObject();
                            if (!inner.isEmpty()) {
                                for (auto iit = inner.begin(); iit != inner.end(); ++iit) {
                                    QJsonObject q = iit.value().toObject();
                                    applySummary(q, summaryFor(
                                        {verb + pit.key() + QLatin1Char('.') + iit.key(),
                                         verb + iit.key()}));
                                    iit.value() = q;
                                }
                                items[QStringLiteral("properties")] = inner;
                                p[QStringLiteral("items")] = items;
                            }
                            pit.value() = p;
                        }
                    };
                    for (int i = 0; i < tools.size(); ++i) {
                        QJsonObject t = tools.at(i).toObject();
                        const QString name =
                            t.value(QStringLiteral("name")).toString();
                        QJsonObject schema =
                            t.value(QStringLiteral("inputSchema")).toObject();
                        QJsonObject props =
                            schema.value(QStringLiteral("properties")).toObject();
                        if (t.value(QStringLiteral("_meta")).toObject()
                                 .value(QStringLiteral("anthropic/alwaysLoad"))
                                 .toBool()
                            || kLeanVerbs.contains(name)) {
                            shorten(props, name);
                            for (const QString &arg : kWireHidden.value(name))
                                props.remove(arg);
                        } else {
                            // ANTS-5714 — every other verb: only the
                            // arguments that mean the same on every verb.
                            // A verb-specific caller_cwd wording is kept;
                            // only the generic dispatcher text is replaced.
                            for (auto pit = props.begin(); pit != props.end(); ++pit) {
                                const QString &arg = pit.key();
                                QJsonObject p = pit.value().toObject();
                                const bool generic =
                                    arg == QLatin1String("caller_cwd")
                                        ? p.value(QStringLiteral("description")).toString()
                                              .contains(QLatin1String("Required by the dispatcher"))
                                        : kSharedArgs.contains(arg);
                                if (!generic) continue;
                                applySummary(p, kArgSummary.value(arg));
                                pit.value() = p;
                            }
                        }
                        schema[QStringLiteral("properties")] = props;
                        t[QStringLiteral("inputSchema")] = schema;
                        const QString desc = kEagerDescription.value(name);
                        if (!desc.isEmpty())
                            t[QStringLiteral("description")] =
                                QStringLiteral("[%1] %2 %3 via tool_info {name:\"%4\"}.")
                                    .arg(kindForName(name), desc,
                                         kWireHidden.contains(name)
                                             ? QStringLiteral("Rarer arguments and full per-op detail")
                                             : QStringLiteral("Full per-op detail"),
                                         name);
                        tools.replace(i, t);
                    }
                }

                // ANTS-1502 — two-tier discovery. When the caller
                // requests `_meta.shape == "lite"`, return a compact
                // shape `[{name, summary, kind}]` per tool instead of
                // the full descriptor set. Callers drill into a
                // specific tool via `tool_info(name)` (ANTS-1399).
                // Opt-in only: by default we return the full shape
                // the MCP spec mandates.
                bool wantLite = false;
                {
                    const QJsonObject params =
                        request.value("params").toObject();
                    const QJsonObject metaObj =
                        params.value(QStringLiteral("_meta")).toObject();
                    const QString shape =
                        metaObj.value(QStringLiteral("shape")).toString();
                    if (shape == QLatin1String("lite")) wantLite = true;
                }
                if (wantLite) {
                    // ANTS-1518 — kindForName is hoisted to the outer
                    // tools/list scope (above) so the full-shape
                    // description-prefix loop and this lite-shape branch
                    // share one source of truth.
                    auto liteSummaryFor = [](const QJsonObject &t) -> QString {
                        // selection_hint when there is one, else the first
                        // sentence of description, cut to 160 characters.
                        const QString hint =
                            t.value(QStringLiteral("selection_hint"))
                             .toString();
                        if (!hint.isEmpty()) return hint;
                        QString desc =
                            t.value(QStringLiteral("description"))
                             .toString();
                        const int dot = desc.indexOf(QChar('.'));
                        if (dot > 0 && dot < 200) {
                            desc = desc.left(dot + 1);
                        }
                        if (desc.size() > 160) {
                            desc = desc.left(157) + QStringLiteral("...");
                        }
                        return desc;
                    };
                    QJsonArray lite;
                    for (const auto &v : std::as_const(tools)) {
                        const QJsonObject t = v.toObject();
                        const QString name =
                            t.value(QStringLiteral("name")).toString();
                        QJsonObject e;
                        e[QStringLiteral("name")]    = name;
                        e[QStringLiteral("summary")] = liteSummaryFor(t);
                        e[QStringLiteral("kind")]    = kindForName(name);
                        lite.append(e);
                    }
                    result[QStringLiteral("tools")] = lite;
                    result[QStringLiteral("shape")] = QStringLiteral("lite");
                    result[QStringLiteral("hint")]  = QStringLiteral(
                        "Lite shape (ANTS-1502): each entry has only "
                        "{name, summary, kind}. Call tool_info(name) "
                        "for the full schema before invoking.");
                } else {
                    result["tools"] = tools;
                }
                haveResult = true;
            } else if (method == "tools/call") {
                QJsonObject params = request.value("params").toObject();
                QString toolName = params.value("name").toString();

                // ANTS-1284 — hoist args + responseText out of the
                // branches so a single recordCall covers both the
                // inline get_session_info path and registered providers.
                const QJsonObject argsObj = mcp::coerceArgTypes(params.value("arguments").toObject(),
                                                                m_toolParamSchemas.value(toolName));
                QString responseText;
                bool toolHandled = false;
                // ANTS-1356 — dispatchResult overrides the default
                // "ok" passed to recordDispatch. Refusal branches set
                // this to surface the refusal in mcp_trace +
                // token_usage's failed-call accumulator.
                // ANTS-1454 retrofitted the ANTS-1404 branch below to
                // set "caller_cwd_required" — the pre-1454 default-to-
                // ok masked the per-call cost of misconfigured callers
                // (see docs/specs/ANTS-1454-caller-cwd-required-refusals-count.md).
                QString dispatchResult = QStringLiteral("ok");
                // ANTS-1360 — start latency clock before cache lookup
                // so cache-hit records capture true µs-range latency
                // (not zero). Stop right before recordMcpTrace below.
                QElapsedTimer mcpTraceTimer;
                mcpTraceTimer.start();
                // The reply pipeline's context, from this call's locals as
                // they stand when it is built.
                const auto baseContext = [&] {
                    McpCallContext c;
                    c.socket         = guard;
                    c.requestId      = reqId;
                    c.toolName       = toolName;
                    c.args           = argsObj;
                    c.requestBytes   = buf.size();
                    c.dispatchResult = dispatchResult;
                    c.traceTimer     = mcpTraceTimer;
                    return c;
                };
                // ANTS-1901 — master MCP gate. When the integration is
                // toggled off at runtime the socket stays bound until the
                // next launch, but observation must stop now: refuse every
                // verb with `mcp_disabled` as the FIRST gate — before the
                // caller_cwd contract check and the idempotent-read cache
                // lookup — so a disabled session never serves even a
                // memoised response. setMcpEnabled() drives m_mcpEnabled
                // from MainWindow on the live Settings toggle; at startup a
                // disabled master never binds the socket, so this branch is
                // only reachable in the runtime toggle-off window.
                if (!m_mcpEnabled) {
                    QJsonObject env;
                    env["ok"]    = false;
                    env["code"]  = QStringLiteral("mcp_disabled");
                    env["error"] = QStringLiteral(
                        "Ants MCP integration is disabled (Settings → "
                        "General → \"Enable Ants MCP integration\"). "
                        "Re-enable it to use this tool.");
                    responseText = QString::fromUtf8(
                        QJsonDocument(env).toJson(QJsonDocument::Compact));
                    toolHandled    = true;
                    dispatchResult = QStringLiteral("mcp_disabled");
                }
                // ANTS-1404 — per-tool caller_cwd contract check.
                // Runs BEFORE the cache lookup so a refused call
                // doesn't pollute the cache, and BEFORE the
                // get_session_info inline branch so the contract
                // applies uniformly. INV-2: tools classified Required
                // refuse with {ok:false, code:"caller_cwd_required"}
                // when caller_cwd is absent — closes the silent
                // focused-fallback leak (2026-05-15 cross-session
                // report). Other categories (Optional, TabSpecific,
                // ProcessGlobal) are no-ops in Phase 3a.
                // ANTS-1419 — consult the stored contract on the
                // registered entry when present (single source of
                // truth at the call site); fall back to the static
                // table for inline-dispatched tools
                // (get_session_info, tool_info) that don't appear
                // in m_toolProviders.
                CallerCwdContract contract;
                if (auto it = m_toolProviders.find(toolName);
                    it != m_toolProviders.end()) {
                    contract = it->second.contract;
                } else {
                    contract = callerCwdContractFor(toolName);
                }
                const QString callerCwd =
                    argsObj.value(QStringLiteral("caller_cwd"))
                        .toString();
                // ANTS-1772 — resolve tool existence BEFORE the
                // caller_cwd contract gate (default Required), else an
                // unknown tool with no caller_cwd would wrongly refuse
                // `caller_cwd_required` instead of `tool_not_found`.
                // Inline tools (get_session_info, tool_info) aren't in
                // m_toolProviders; token_usage / mcp_trace are. Gating
                // contract + rate-limit on toolKnown lets an unknown tool
                // fall through to the registry-miss else-branch below.
                const bool toolKnown =
                    toolName == QStringLiteral("get_session_info") ||
                    toolName == QStringLiteral("tool_info") ||
                    m_toolProviders.find(toolName) != m_toolProviders.end() ||
                    m_forwardedTools.contains(toolName);  // ANTS-4932 § 2.5
                if (!toolHandled && toolKnown &&
                    contract == CallerCwdContract::Required &&
                    callerCwd.isEmpty()) {
                    // ANTS-1853 — distinguish "the whole arguments object
                    // arrived empty" (the call's parameters were dropped in
                    // transit — an intermittent tools/call serialisation drop
                    // observed 2026-05-25) from "caller_cwd was the only
                    // missing field". The former is NOT a caller error:
                    // re-adding caller_cwd alone can't help because every
                    // other arg is gone too. Flagging it steers the caller to
                    // resend the ENTIRE call rather than fixate on one field,
                    // and the diagnostic log below lets a recurrence be root-
                    // caused against the Claude Code integration debug lane.
                    const bool argumentsEmpty = argsObj.isEmpty();
                    QJsonObject env;
                    env["ok"]    = false;
                    env["code"]  = QStringLiteral("caller_cwd_required");
                    env["error"] = QString(
                        "%1: caller_cwd is required for this tool "
                        "(ANTS-1404). Pass your $PWD as caller_cwd "
                        "so the tool routes to your project rather "
                        "than whichever tab Ants has focused.")
                        .arg(toolName)
                        // ANTS-1857 — size-aware steer: name the
                        // large-payload root cause + the two mitigations
                        // (shrink / use Edit), not just "resend".
                        + (argumentsEmpty
                            ? QStringLiteral(
                                " NOTE: this call arrived with NO arguments "
                                "at all — if you DID pass caller_cwd, the "
                                "whole parameter payload was dropped in "
                                "transit (the data never reached Ants; not "
                                "an Ants bug). Resend the entire call "
                                "verbatim. If it keeps dropping, the payload "
                                "is likely too large — shrink it (e.g. a "
                                "shorter roadmap_log `body`, or split the "
                                "work) or write the content with the Edit "
                                "tool directly. Small calls are reliable "
                                "(ANTS-1853).")
                            : QString());
                    // ANTS-1418 — surface the diagnostic verb so a
                    // caller who already passes caller_cwd but still
                    // gets the wrong tab (symlinked roots, worktree
                    // checkouts, container bind-mounts) finds the
                    // tool that lets them confirm the resolution.
                    env["hint"] = QStringLiteral(
                        "call mcp__ants__caller_cwd_info with your "
                        "$PWD to confirm which tab Ants would route "
                        "this call to");
                    // ANTS-1543 — concrete JSON snippet (mirrors the
                    // session_memory + RcGate envelopes) so the
                    // caller can copy the exact arguments shape.
                    QJsonObject ex;
                    ex[QStringLiteral("caller_cwd")] =
                        QStringLiteral("<your $PWD>");
                    env[QStringLiteral("example")] = ex;
                    // ANTS-1853 — machine-readable flag for the dropped-
                    // payload case so a caller (or test) can branch on it.
                    if (argumentsEmpty)
                        env[QStringLiteral("arguments_empty")] = true;
                    // ANTS-1853 — diagnostic trace on every caller_cwd
                    // refusal. The parsed-key fields alone can't tell a
                    // truncated-transport drop from an intact request whose
                    // `arguments` serialised empty upstream, so also log the
                    // raw whole-request byte length (`buf`), whether the
                    // `arguments` key was present at all, and a bounded
                    // request fingerprint (the "raw length + content hash" the
                    // ROADMAP note asked for). See ANTS-1853 for the matrix.
                    ANTS_LOG(DebugLog::Claude,
                             "ANTS-1853 tools/call refused caller_cwd_required:"
                             " tool=%s arguments_empty=%d args_key_present=%d"
                             " arg_keys=%d req_bytes=%d req_hash=%u keys=[%s]",
                             toolName.toUtf8().constData(),
                             argumentsEmpty ? 1 : 0,
                             params.contains(QStringLiteral("arguments")) ? 1 : 0,
                             static_cast<int>(argsObj.size()),
                             static_cast<int>(buf.size()),
                             static_cast<unsigned>(qHash(buf)),
                             argsObj.keys().join(QLatin1Char(','))
                                 .toUtf8().constData());
                    responseText = QString::fromUtf8(
                        QJsonDocument(env)
                            .toJson(QJsonDocument::Compact));
                    toolHandled    = true;
                    // ANTS-1454 — route the refusal to recordDispatch's
                    // failed-call accumulator. Pre-1454 the branch
                    // inherited the "ok" default and token_usage
                    // double-counted refusals as successes.
                    dispatchResult = QStringLiteral("caller_cwd_required");
                }
                // ANTS-1415 — Phase 3b: TabSpecific contract enforcement.
                // The per-tab read tools route on caller_cwd (ANTS-1392)
                // and, for those tabSpecificAcceptsTabIndex lists, an
                // explicit `tab` index. With NO usable routing key they
                // fall back to the focused tab — the cross-tenant leak
                // ANTS-1404 closed for Required tools, deferred from
                // Phase 3a because the routing-vs-anchoring semantics
                // needed their own spec pass. Refuse before the
                // rate-limit + cache, mirroring the Required branch.
                // A stray `tab` on a cwd-only tool is NOT a routing key
                // (the handler ignores it), else it would bypass the
                // gate and still fall back to focused. See
                // docs/standards/mcp-error-codes.md § 3 + docs/specs/ANTS-1415-callercwdcontract-tabspecific.md.
                if (!toolHandled && toolKnown &&
                    contract == CallerCwdContract::TabSpecific) {
                    const bool tabIsRoutingKey =
                        tabSpecificAcceptsTabIndex(toolName);
                    const bool hasTab = tabIsRoutingKey &&
                        argsObj.value(QStringLiteral("tab")).isDouble();
                    if (callerCwd.isEmpty() && !hasTab) {
                        QJsonObject env;
                        env["ok"]   = false;
                        env["code"] = QStringLiteral("tab_or_cwd_required");
                        env["error"] = (tabIsRoutingKey
                            ? QString("%1: pass a `tab` index or `caller_cwd` "
                                      "(your $PWD) to identify the target tab "
                                      "(ANTS-1415). Without either, this "
                                      "per-tab tool falls back to whichever tab "
                                      "Ants has focused — which may belong to a "
                                      "different project.")
                            : QString("%1: pass `caller_cwd` (your $PWD) to "
                                      "identify the target tab (ANTS-1415). "
                                      "Without it, this per-tab tool falls back "
                                      "to whichever tab Ants has focused — which "
                                      "may belong to a different project."))
                            .arg(toolName);
                        env["hint"] = QStringLiteral(
                            "call mcp__ants__caller_cwd_info with your $PWD to "
                            "confirm which tab Ants would route this call to; "
                            "tab_list gives tab indices");
                        QJsonObject ex;
                        ex[QStringLiteral("caller_cwd")] =
                            QStringLiteral("<your $PWD>");
                        env[QStringLiteral("example")] = ex;
                        responseText = QString::fromUtf8(
                            QJsonDocument(env).toJson(QJsonDocument::Compact));
                        toolHandled    = true;
                        dispatchResult = QStringLiteral("tab_or_cwd_required");
                    }
                }
                // ANTS-4932 § 2.3 — ants-mcpd forwards a terminal-scoped verb
                // whole once the gates above have passed it, and emits the
                // terminal's reply unchanged: rate limit, cache, ETag, fields=
                // and the wrap all ran in the terminal. A forwarder failure is
                // a no_terminal envelope, finished like any other refusal.
                // Never adds a caller_cwd: the request goes as it came (INV-8).
                if (!toolHandled && m_forwarder &&
                    m_forwardedTools.contains(toolName)) {
                    McpCallContext fctx = baseContext();
                    fctx.toolHandled = true;
                    const QPointer<ClaudeIntegration> self(this);
                    m_forwarder(buf,
                        [fctx](const QByteArray &replyLine) {
                            McpReplyChannel *const channel = fctx.socket.data();
                            if (channel && channel->isOpen())
                                channel->reply(replyLine);
                        },
                        [self, fctx](const QString &envelope) {
                            if (!self) return;
                            self->finishToolDispatch(
                                fctx, transformReply(fctx, envelope));
                        });
                    return;
                }
                // ANTS-1356 — per-tool sliding-window rate-limit.
                // Runs AFTER caller_cwd_required (a misconfigured
                // caller should see the precise refusal first) but
                // BEFORE the idempotent-read cache lookup (cache
                // hits consume bucket budget — INV-5). Refusal sets
                // toolHandled=true with a {ok:false, code:"rate_limited",
                // retry_after_ms} envelope; downstream wrap + record
                // paths see it like any other completed call.
                if (!toolHandled && toolKnown) {
                    const qint64 nowMs = s_rateLimitClock.elapsed();
                    const qint64 retryAfter =
                        rateLimitCheck(toolName, callerCwd, nowMs);
                    if (retryAfter > 0) {
                        const RateLimitTier tier =
                            rateLimitTierFor(rateLimitClassFor(toolName));
                        QJsonObject env;
                        env["ok"]              = false;
                        env["code"]            = QStringLiteral("rate_limited");
                        env["retry_after_ms"]  = static_cast<qint64>(retryAfter);
                        env["error"] = QString(
                            "%1: rate-limited (%2 calls in last "
                            "%3 s, cap %2). retry_after_ms=%4")
                            .arg(toolName)
                            .arg(tier.capPerWindow)
                            .arg(tier.windowMs / 1000)
                            .arg(retryAfter);
                        responseText = QString::fromUtf8(
                            QJsonDocument(env)
                                .toJson(QJsonDocument::Compact));
                        toolHandled    = true;
                        dispatchResult = QStringLiteral("rate_limited");
                    }
                }
                // ANTS-1357 — short-TTL idempotent-read cache lookup
                // happens before dispatch. Only the 4 allowlisted tools
                // hit this path (isIdempotentReadTool); everything else
                // falls through to the registry below. INV-1: cache
                // hit response is byte-identical to a miss at stampMs.
                bool cachedHit = false;
                // Cleared on the dispatch_queue_full branch below.
                bool cacheable =
                    !toolHandled && isIdempotentReadTool(toolName);
                // baseContext plus the cache state; a handled call also
                // carries its ignored-arg keys.
                const auto dispatchContext = [&](bool handled) {
                    McpCallContext c = baseContext();
                    c.cachedHit   = cachedHit;
                    c.cacheable   = cacheable;
                    c.toolHandled = handled;
                    if (handled)
                        c.ignoredArgKeys = ignoredArgKeysFor(toolName, argsObj);
                    return c;
                };
                if (cacheable) {
                    const QString hit = tryGetIdempotentReadCache(
                        toolName, argsObj);
                    if (!hit.isEmpty()) {
                        responseText = hit;
                        toolHandled  = true;
                        cachedHit    = true;
                    }
                }
                // ANTS-1253: get_session_info stays inline — it reads
                // ClaudeIntegration's own private state (m_state,
                // m_currentTool, m_contextPercent, m_changedFiles,
                // m_activeSessionId) rather than delegating to an
                // external provider. All outward-delegate tools
                // dispatch via the registry below. Guard widened to
                // `!toolHandled` so a prior refusal (ANTS-1404
                // contract check) or cache hit (ANTS-1357) short-
                // circuits this block.
                if (!toolHandled) {
                    if (toolName == "get_session_info") {
                        QJsonObject info;
                        info["state"] = static_cast<int>(m_state);
                        info["current_tool"] = m_currentTool;
                        info["context_percent"] = m_contextPercent;
                        info["changed_files"] = QJsonArray::fromStringList(m_changedFiles);
                        info["session_id"] = m_activeSessionId;
                        // ANTS-1952 — re-surface build identity here too, so a
                        // session that has already handshaked can confirm the
                        // running binary's SHA without re-issuing initialize.
                        info["server_build_commit"] = QString::fromLatin1(ANTS_BUILD_COMMIT);  // ANTS-3582: extern arrays
                        info["server_build_date"] = QString::fromLatin1(ANTS_BUILD_DATE);
                        info["server_build_time"] = QString::fromLatin1(ANTS_BUILD_TIME);
                        info["server_build_type"] = QString::fromLatin1(ANTS_BUILD_TYPE);
                        responseText = QString::fromUtf8(
                            QJsonDocument(info).toJson(QJsonDocument::Compact));
                        toolHandled = true;
                    } else if (toolName == "tool_info") {
                        // ANTS-1399-INV-3 — read from the snapshot
                        // populated at tools/list end. Inline-dispatched
                        // (no provider lambda) so the cache lives next
                        // to the descriptors it slices from.
                        QJsonObject env;
                        const QString reqName =
                            argsObj.value(QStringLiteral("name")).toString();
                        // ANTS-1985 — catalog mode. Evaluated BEFORE the
                        // reqName.isEmpty() guard: catalog calls supply no
                        // `name`, so reaching missing_name first would
                        // wrongly refuse every catalog request (spec § 2.4).
                        const bool catalogMode =
                            argsObj.value(QStringLiteral("catalog")).toBool();
                        if (catalogMode) {
                            if (m_lastToolsList.isEmpty()) {
                                env["ok"]    = false;
                                env["code"]  = QStringLiteral("tools_not_ready");
                                env["error"] = QStringLiteral(
                                    "tool_info: call tools/list first to "
                                    "populate the descriptor cache");
                            } else {
                                // Group by the [<kind>] description prefix
                                // (injected at tools/list, ANTS-1518). QMap
                                // keys sort ascending, so both categories
                                // and the names within each emit in stable
                                // sorted order (INV-12). A missing or
                                // malformed prefix ([ unclosed, or []) →
                                // "other", keeping the parse total.
                                auto categoryOf =
                                    [](const QString &desc) -> QString {
                                    if (desc.startsWith(QLatin1Char('['))) {
                                        const int close =
                                            desc.indexOf(QLatin1Char(']'));
                                        if (close > 1)
                                            return desc.mid(1, close - 1);
                                    }
                                    return QStringLiteral("other");
                                };
                                QMap<QString, QMap<QString, QString>> grouped;
                                for (const auto &v :
                                         std::as_const(m_lastToolsList)) {
                                    const QJsonObject d = v.toObject();
                                    const QString n = d.value(
                                        QStringLiteral("name")).toString();
                                    const QString cat = categoryOf(
                                        d.value(QStringLiteral("description"))
                                         .toString());
                                    grouped[cat][n] = d.value(
                                        QStringLiteral("selection_hint"))
                                        .toString();
                                }
                                QJsonObject catalog;
                                int toolCount = 0;
                                for (auto c = grouped.constBegin();
                                     c != grouped.constEnd(); ++c) {
                                    QJsonArray arr;
                                    for (auto t = c.value().constBegin();
                                         t != c.value().constEnd(); ++t) {
                                        QJsonObject e;
                                        e[QStringLiteral("name")] = t.key();
                                        e[QStringLiteral("selection_hint")] =
                                            t.value();
                                        arr.append(e);
                                        ++toolCount;
                                    }
                                    catalog[c.key()] = arr;
                                }
                                env["ok"]             = true;
                                env["catalog"]        = catalog;
                                env["tool_count"]     = toolCount;
                                env["category_count"] = catalog.size();
                                // ANTS-5807 — said once, on the call a caller
                                // makes before dispatching verbs in parallel.
                                // Each verb is registered on one lane and
                                // each lane is one thread (postWorkerJob).
                                env["concurrency"] = QStringLiteral(
                                    "Within one server, calls to the same verb run "
                                    "one at a time, so parallel calls queue; "
                                    "audit_run refuses already_running for a project "
                                    "it is sweeping. Each session runs its own "
                                    "server, so two sessions' calls can overlap: a "
                                    "roadmap-store write is one transaction, but a "
                                    "verb that rewrites a file (apply_edits) "
                                    "replaces it whole, and the last writer wins.");
                                // ANTS-2073 — stamp the running server's
                                // build identity on the catalog (the
                                // documented once-per-session discovery
                                // call) so a client can self-diagnose a
                                // stale-binary deploy gap without a
                                // separate get_session_info round-trip.
                                QJsonObject sb;
                                sb["version"]      = QStringLiteral(ANTS_VERSION);
                                sb["build_commit"] = QString::fromLatin1(ANTS_BUILD_COMMIT);  // ANTS-3582: extern arrays
                                sb["build_date"]   = QString::fromLatin1(ANTS_BUILD_DATE);
                                sb["build_time"]   = QString::fromLatin1(ANTS_BUILD_TIME);
                                sb["build_type"]   = QString::fromLatin1(ANTS_BUILD_TYPE);
                                env["server_build"] = sb;
                            }
                        } else if (reqName.isEmpty()) {
                            env["ok"]    = false;
                            env["code"]  = QStringLiteral("missing_name");
                            env["error"] = QStringLiteral(
                                "tool_info: 'name' arg is required");
                        } else if (m_lastToolsList.isEmpty()) {
                            env["ok"]    = false;
                            env["code"]  = QStringLiteral("tools_not_ready");
                            env["error"] = QStringLiteral(
                                "tool_info: call tools/list first to "
                                "populate the descriptor cache");
                        } else {
                            QJsonObject match;
                            QJsonArray available;
                            for (const auto &v : std::as_const(m_lastToolsList)) {
                                const QJsonObject d = v.toObject();
                                const QString n =
                                    d.value(QStringLiteral("name")).toString();
                                available.append(n);
                                if (n == reqName) match = d;
                            }
                            if (match.isEmpty()) {
                                env["ok"]        = false;
                                env["code"]      = QStringLiteral("unknown_tool");
                                env["error"]     = QStringLiteral(
                                    "tool_info: no MCP tool registered "
                                    "as '%1'").arg(reqName);
                                env["available"] = available;
                                // ANTS-5485 — a removed verb names its successor.
                                const QString replacement =
                                    mcp::removedReplacement(reqName);
                                if (!replacement.isEmpty())
                                    env["replacement"] = replacement;
                            } else {
                                env["ok"]          = true;
                                env["name"]        = match.value(QStringLiteral("name"));
                                env["description"] = match.value(QStringLiteral("description"));
                                env["inputSchema"] = match.value(QStringLiteral("inputSchema"));
                                // ANTS-1453 — pass selection_hint through
                                // unchanged. No defaulting (every tool sets
                                // it in tools/list per HINT-1).
                                env["selection_hint"] =
                                    match.value(QStringLiteral("selection_hint"));
                                // ANTS-2079 — surface the per-op detail
                                // trimmed from the wire payload. Conditional
                                // (NOT mirroring selection_hint's
                                // unconditional set): present only for tools
                                // that authored one; an unconditional set
                                // would emit detail:null for the ~66 tools
                                // that didn't and break INV-2.
                                if (match.contains(QStringLiteral("detail")))
                                    env["detail"] =
                                        match.value(QStringLiteral("detail"));
                            }
                        }
                        responseText = QString::fromUtf8(
                            QJsonDocument(env).toJson(QJsonDocument::Compact));
                        toolHandled = true;
                    } else if (auto it = m_toolProviders.find(toolName);
                               it != m_toolProviders.end()) {
                        // ANTS-1419 — value type is RegisteredTool
                        // (handler + contract); call the handler.
                        // ANTS-2132 — off-thread where the registration
                        // marked it so. postToolDispatch returns having
                        // queued the work; the reply is written later, from
                        // finishToolDispatch, on this thread.
                        if (it->second.deferred) {
                            // ANTS-2132 § 2.8 — the verb replies later, on
                            // this thread, through the same pipeline.
                            const McpCallContext dctx = dispatchContext(true);
                            const QPointer<ClaudeIntegration> self(this);
                            const auto replied = std::make_shared<bool>(false);
                            it->second.deferred(argsObj,
                                [self, dctx, replied](QString body) {
                                    // The first reply is written; any later
                                    // one is dropped.
                                    if (*replied || !self) return;
                                    *replied = true;
                                    self->finishToolDispatch(
                                        dctx, transformReply(dctx, body));
                                });
                            return;  // the reply follows from the verb
                        }
                        if (it->second.offThread) {
                            if (postToolDispatch(dispatchContext(true), it->second.handler,
                                                 it->second.lane))
                                return;  // reply follows from the worker
                            // Queue full — refuse rather than drop it, so a
                            // caller sees a code instead of a dead socket.
                            QJsonObject env;
                            env["ok"]             = false;
                            env["code"]           = QStringLiteral("dispatch_queue_full");
                            env["error"]          = QStringLiteral(
                                "%1: too many MCP calls are already in "
                                "flight; retry shortly").arg(toolName);
                            env["retry_after_ms"] = 250;
                            responseText   = QString::fromUtf8(
                                QJsonDocument(env).toJson(QJsonDocument::Compact));
                            dispatchResult = QStringLiteral("dispatch_queue_full");
                            // ANTS-5090 — a transient refusal is not an answer
                            // (ANTS-1357 INV-5); caching it would serve it to the
                            // next caller after the queue has drained.
                            cacheable = false;
                        } else {
                            responseText = it->second.handler(argsObj);
                        }
                        toolHandled = true;
                    }
                }
                // ANTS-2132 — hand the rest of the reply to finishToolDispatch.
                // It owns the transforms, the wrap, recordDispatch and the
                // socket write for this path.
                const McpCallContext ctx = dispatchContext(toolHandled);
                finishToolDispatch(ctx, transformReply(ctx, responseText));
                return;
            } else if (method == QLatin1String(VerifyTrust::kPromptMethod)
                       && m_verifyTrustPromptHandler) {
                // ANTS-5464 § 2.2 — not a tool. The handler may hold a prompt
                // open, so it runs on the Bulk worker: the GUI thread and the
                // Shared worker keep serving (INV-9). The reply is written
                // from the GUI thread, like every other.
                const QJsonObject params = request.value("params").toObject();
                if (postWorkerJob([this, guard, reqId, params,
                                   handler = m_verifyTrustPromptHandler]() {
                        const QJsonObject r = handler(params);
                        QMetaObject::invokeMethod(this, [guard, reqId, r]() {
                            sendMcpResponse(guard, reqId, &r, nullptr);
                        }, Qt::QueuedConnection);
                    }, DispatchLane::Bulk))
                    return;
                error["code"] = -32000;
                error["message"] = QStringLiteral(
                    "%1: too many MCP calls are already in flight; retry shortly")
                    .arg(method);
            } else {
                // JSON-RPC -32601 = Method not found
                error["code"] = -32601;
                error["message"] = QString("Method not found: %1").arg(method);
            }

            sendMcpResponse(guard, reqId,
                            haveResult ? &result : nullptr,
                            haveResult ? nullptr : &error);
        }
    }
}
