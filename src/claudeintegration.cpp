#include "claudeintegration.h"

#include "secureio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QCoreApplication>

ClaudeIntegration::ClaudeIntegration(QObject *parent) : QObject(parent) {
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
    stopHookServer();
    stopMcpServer();
}

// --- Process Detection ---

void ClaudeIntegration::setShellPid(pid_t pid) {
    // 0.6.22 — on tab switch, the caller hands us the new tab's shell PID.
    // Without this clear, the cached m_state / m_currentTool / context%
    // from the previous tab persisted until the next poll tick (~1s
    // later), causing the user-reported "Claude status indicator doesn't
    // work half the time" symptom — tab A's "Claude: thinking..." bled
    // into tab B. Reset state immediately when the PID changes so the UI
    // reflects the tab switch within the current event-loop iteration.
    // Same-PID calls (rebind on identical shell) are idempotent.
    if (pid != m_shellPid) {
        m_state = ClaudeState::NotRunning;
        m_currentTool.clear();
        m_contextPercent = 0;
        m_claudePid = 0;            // force pollClaudeProcess to re-detect
        m_activeSessionId.clear();
        if (m_planMode) { m_planMode = false; emit planModeChanged(false); }
        if (m_auditing) { m_auditing = false; emit auditingChanged(false); }
        if (!m_transcriptPath.isEmpty()) {
            m_transcriptWatcher.removePath(m_transcriptPath);
            m_transcriptPath.clear();
        }
        emit stateChanged(ClaudeState::NotRunning, QString());
        emit contextUpdated(0);
    }
    m_shellPid = pid;
    if (pid > 0) {
        m_pollTimer.start();
        // Run one poll immediately so the Claude status label shows the
        // correct state for the new tab within the current event-loop
        // iteration rather than "NotRunning" for up to 2 s until the
        // next timer tick. Without this, tab-switching to a tab where
        // Claude IS running briefly reads as "Claude: not running,"
        // which the user sees as the status bar being inaccurate.
        pollClaudeProcess();
    } else {
        m_pollTimer.stop();
    }
}

void ClaudeIntegration::pollClaudeProcess() {
    if (m_shellPid <= 0) return;

    // Scan only direct children of our shell via /proc/<pid>/task/<tid>/children
    bool found = false;
    pid_t foundPid = 0;

    // Read child PIDs from the kernel's children file (much faster than scanning all /proc)
    QFile childFile(QString("/proc/%1/task/%1/children").arg(m_shellPid));
    QList<pid_t> childPids;
    if (childFile.open(QIODevice::ReadOnly)) {
        QString children = QString::fromUtf8(childFile.readAll()).trimmed();
        childFile.close();
        for (const QString &pidStr : children.split(' ', Qt::SkipEmptyParts)) {
            bool ok;
            pid_t pid = pidStr.toInt(&ok);
            if (ok && pid > 0) childPids.append(pid);
        }
    } else {
        // Fallback: scan /proc but only check stat for ppid match
        QDir procDir("/proc");
        for (const QString &entry : procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            bool ok;
            pid_t pid = entry.toInt(&ok);
            if (!ok || pid <= 0) continue;

            QFile statFile(QString("/proc/%1/stat").arg(pid));
            if (!statFile.open(QIODevice::ReadOnly)) continue;
            QString stat = QString::fromUtf8(statFile.readAll());
            statFile.close();

            int closeParenIdx = stat.lastIndexOf(')');
            if (closeParenIdx < 0) continue;
            QStringList fields = stat.mid(closeParenIdx + 2).split(' ');
            if (fields.size() < 2) continue;
            pid_t ppid = fields[1].toInt();
            if (ppid == m_shellPid) childPids.append(pid);
        }
    }

    // Match the executable, not any substring. "grep claude file" or a user
    // with "~/bin/claude-search" must NOT be mistaken for Claude Code.
    auto basename = [](const QString &path) -> QString {
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? path.mid(slash + 1) : path;
    };
    auto isClaudeBin = [](const QString &name) {
        return name == QLatin1String("claude") || name == QLatin1String("claude-code");
    };

    for (pid_t pid : childPids) {
        QFile cmdFile(QString("/proc/%1/cmdline").arg(pid));
        if (!cmdFile.open(QIODevice::ReadOnly)) continue;
        QByteArray raw = cmdFile.readAll();
        cmdFile.close();
        // /proc/<pid>/cmdline is NUL-separated argv.
        QList<QByteArray> argv = raw.split('\0');
        // Trailing empty element from the final NUL — drop it if present.
        while (!argv.isEmpty() && argv.last().isEmpty()) argv.removeLast();
        if (argv.isEmpty()) continue;

        QString arg0 = basename(QString::fromUtf8(argv.first()));
        bool match = isClaudeBin(arg0);

        // Node/deno/bun launchers: inspect argv[1..] for a script basename.
        if (!match && (arg0 == QLatin1String("node") ||
                       arg0 == QLatin1String("deno") ||
                       arg0 == QLatin1String("bun"))) {
            for (qsizetype i = 1; i < argv.size(); ++i) {
                QString scriptName = basename(QString::fromUtf8(argv[i]));
                // Strip a trailing .js/.mjs/.cjs/.ts so "cli.js" and similar
                // don't prevent a match — though typically the interesting
                // basename is the parent directory, not the script. We match
                // on the path containing "/claude/" or "/claude-code/".
                QString full = QString::fromUtf8(argv[i]);
                if (isClaudeBin(scriptName) ||
                    full.contains(QLatin1String("/claude-code/")) ||
                    full.contains(QLatin1String("/claude/"))) {
                    match = true;
                    break;
                }
            }
        }

        if (match) {
            found = true;
            foundPid = pid;
            break;
        }
    }

    if (!found) {
        m_claudePid = 0;
        m_transcriptPath.clear();
        if (m_state != ClaudeState::NotRunning) {
            m_state = ClaudeState::NotRunning;
            emit stateChanged(m_state, m_currentTool);
        }
        return;
    }

    if (m_claudePid == 0) {
        // Newly detected Claude process
        m_claudePid = foundPid;

        // Find the most recently modified transcript across ALL projects
        QString home = QDir::homePath();
        QDir claudeDir(home + "/.claude/projects");
        if (claudeDir.exists()) {
            QFileInfo newest;
            for (const QString &projDir : claudeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QDir proj(claudeDir.filePath(projDir));
                for (const QFileInfo &fi : proj.entryInfoList({"*.jsonl"}, QDir::Files)) {
                    if (!newest.exists() || fi.lastModified() > newest.lastModified())
                        newest = fi;
                }
            }
            if (newest.exists()) {
                m_transcriptPath = newest.absoluteFilePath();

                // Swap watch to the new transcript. Signal hookup happens
                // once in the constructor, so no disconnect/connect dance.
                QStringList oldFiles = m_transcriptWatcher.files();
                if (!oldFiles.isEmpty())
                    m_transcriptWatcher.removePaths(oldFiles);
                m_transcriptWatcher.addPath(m_transcriptPath);

                // Seed state from the current transcript tail — without this
                // the UI would show "Idle" until the next write event fires.
                parseTranscriptForState(m_transcriptPath);
            }
        }

        // Set initial Idle state, then let transcript parse refine it
        if (m_state != ClaudeState::Idle) {
            m_state = ClaudeState::Idle;
            emit stateChanged(m_state, "idle");
        }
    }

    // Backstop re-parse: event-driven path handles ~99% of updates, but a
    // file-replaced event can unbind the inotify watch in edge cases. Once
    // every 10 poll cycles (~20s) we re-parse unconditionally — parse is
    // cheap (~32KB read) and this also re-arms the watch via the
    // addPath-if-missing check at the top of parseTranscriptForState.
    if (!m_transcriptPath.isEmpty() && ++m_transcriptBackstopTicks >= 10) {
        m_transcriptBackstopTicks = 0;
        parseTranscriptForState(m_transcriptPath);
    }
}

// --- Session Transcripts ---

QString ClaudeIntegration::activeSessionPath() const {
    QString home = QDir::homePath();
    QDir claudeDir(home + "/.claude/projects");
    if (!claudeDir.exists()) return {};

    QFileInfo newest;
    for (const QString &projDir : claudeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir proj(claudeDir.filePath(projDir));
        for (const QFileInfo &fi : proj.entryInfoList({"*.jsonl"}, QDir::Files, QDir::Time)) {
            if (!newest.exists() || fi.lastModified() > newest.lastModified())
                newest = fi;
        }
    }
    return newest.absoluteFilePath();
}

QJsonArray ClaudeIntegration::loadTranscript(const QString &path) const {
    QJsonArray entries;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return entries;
    // Skip excessively large transcripts to prevent memory exhaustion
    if (file.size() > 100 * 1024 * 1024) return entries;

    while (!file.atEnd()) {
        QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            entries.append(doc.object());
    }
    return entries;
}

QStringList ClaudeIntegration::recentSessions() const {
    QStringList sessions;
    QString home = QDir::homePath();
    QDir claudeDir(home + "/.claude/projects");
    if (!claudeDir.exists()) return sessions;

    for (const QString &projDir : claudeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir proj(claudeDir.filePath(projDir));
        for (const QFileInfo &fi : proj.entryInfoList({"*.jsonl"}, QDir::Files, QDir::Time)) {
            sessions.append(fi.absoluteFilePath());
            if (sessions.size() >= 20) return sessions;
        }
    }
    return sessions;
}

void ClaudeIntegration::parseTranscriptForState(const QString &path) {
    // Re-add the watch — QFileSystemWatcher can drop it after atomic saves
    if (!m_transcriptWatcher.files().contains(path))
        m_transcriptWatcher.addPath(path);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;

    // Read the last 32KB of the transcript. Claude Code writes several
    // metadata-only events at the end of each turn (`system/turn_duration`,
    // `last-prompt`, `permission-mode`, `file-history-snapshot`, `summary`),
    // so we need enough buffer to walk back past them to the real
    // state-determining event (`assistant`/`user`/`attachment`).
    qint64 size = file.size();
    constexpr qint64 kWindow = 32768;
    if (size > kWindow) file.seek(size - kWindow);

    QList<QJsonObject> events;
    bool firstLine = (size > kWindow); // first line after seek is likely truncated
    while (!file.atEnd()) {
        QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) continue;
        if (firstLine) {
            firstLine = false;
            continue;
        }
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) events.append(doc.object());
    }

    if (events.isEmpty()) return;

    // Metadata event types that don't affect run state. They can appear after
    // `assistant/end_turn`, so naively looking at only the last event would
    // miss the real terminal state of the turn.
    static const QSet<QString> kMetadataTypes = {
        QStringLiteral("system"),
        QStringLiteral("last-prompt"),
        QStringLiteral("permission-mode"),
        QStringLiteral("file-history-snapshot"),
        QStringLiteral("summary"),
        QStringLiteral("meta"),
    };

    // Update context% from the most recent event carrying usage info. Walk
    // backward — `assistant` events have a `message.usage.input_tokens` field.
    for (int i = events.size() - 1; i >= 0; --i) {
        QJsonObject usage = events[i].value("message").toObject()
                                     .value("usage").toObject();
        int inputTokens = usage.value("input_tokens").toInt();
        if (inputTokens > 0) {
            // Rough estimate: 200K context window
            m_contextPercent = std::min(100, inputTokens * 100 / 200000);
            emit contextUpdated(m_contextPercent);
            break;
        }
    }

    // Find the most recent state-determining event by walking backward.
    QJsonObject stateEvent;
    for (int i = events.size() - 1; i >= 0; --i) {
        QString t = events[i].value("type").toString();
        if (!kMetadataTypes.contains(t)) {
            stateEvent = events[i];
            break;
        }
    }
    if (stateEvent.isEmpty()) return;

    QString type = stateEvent.value("type").toString();
    ClaudeState newState = m_state;
    QString detail;

    if (type == "assistant") {
        QJsonObject msg = stateEvent.value("message").toObject();
        QString stopReason = msg.value("stop_reason").toString();
        QJsonArray content = msg.value("content").toArray();

        // Detect tool_use in content blocks (more reliable than stop_reason alone)
        QString toolName;
        bool hasToolUse = false;
        for (const QJsonValue &c : content) {
            QJsonObject block = c.toObject();
            if (block.value("type").toString() == "tool_use") {
                hasToolUse = true;
                toolName = block.value("name").toString();
                updateChangedFiles(block);
                break;
            }
        }

        if (hasToolUse || stopReason == "tool_use") {
            newState = ClaudeState::ToolUse;
            m_currentTool = toolName;
            detail = toolName.isEmpty() ? QStringLiteral("tool use") : toolName;
        } else if (stopReason.isEmpty() || stopReason == "null") {
            // Still streaming — stop_reason not yet finalized
            newState = ClaudeState::Thinking;
            detail = "thinking";
        } else {
            // end_turn, max_tokens, stop_sequence, refusal → waiting for user
            newState = ClaudeState::Idle;
            detail = "idle";
        }
    } else if (type == "user" || type == "human") {
        // `user` wraps both real user messages and tool_result entries. Either
        // way Claude is processing — state is Thinking.
        QJsonValue content = stateEvent.value("message").toObject().value("content");
        bool isToolResult = false;
        if (content.isArray()) {
            for (const QJsonValue &c : content.toArray()) {
                if (c.toObject().value("type").toString() == "tool_result") {
                    isToolResult = true;
                    break;
                }
            }
        }
        newState = ClaudeState::Thinking;
        detail = isToolResult ? QStringLiteral("processing result")
                              : QStringLiteral("thinking");
    } else if (type == "attachment") {
        newState = ClaudeState::Thinking;
        detail = "thinking";
    }
    // Any other state-determining type we don't recognize: leave state unchanged.

    // /compact override. The command is recorded in the transcript as a user
    // event with string content "<command-name>/compact</command-name>...".
    // Compaction completes when Claude writes another user event carrying the
    // condensed history with `isCompactSummary:true`. While the former is the
    // most recent real user message and no matching summary has followed,
    // surface Compacting — otherwise the UI just says "thinking..." for the
    // entire (often multi-second) summarization turn. Walks the same 32KB
    // window we already read; if an old /compact falls outside the window we
    // silently fall back to the generic state, which is acceptable.
    bool inCompact = false;
    // Auditing: mirrors the /compact pattern. The /audit skill is
    // invoked by the user as `<command-name>/audit</command-name>`. We
    // treat it as "active for the rest of the conversation turn it was
    // invoked in" — i.e. until the assistant's next `end_turn` stop
    // reason. This matches the user's mental model ("Claude is auditing
    // until the audit is reported") without needing to model the skill's
    // internal state machine.
    bool inAudit = false;
    for (int i = events.size() - 1; i >= 0; --i) {
        if (events[i].value("type").toString() != QLatin1String("user")) continue;
        // Found an already-completed compact first → nothing in flight.
        if (events[i].value("isCompactSummary").toBool()) break;
        QJsonValue content = events[i].value("message").toObject().value("content");
        if (!content.isString()) continue;  // tool_result arrays, skip
        const QString contentStr = content.toString();
        if (contentStr.contains(
                QStringLiteral("<command-name>/compact</command-name>"))) {
            inCompact = true;
        }
        if (contentStr.contains(
                QStringLiteral("<command-name>/audit</command-name>"))) {
            inAudit = true;
        }
        break;  // first genuine user message decides
    }
    // Audit latches off when the assistant hits end_turn after the /audit
    // user message — walk forward from the /audit point and see if any
    // assistant event after it has stop_reason == "end_turn". If so, the
    // audit turn is complete.
    if (inAudit) {
        bool auditFinished = false;
        bool pastAudit = false;
        for (const QJsonObject &ev : events) {
            if (!pastAudit) {
                if (ev.value("type").toString() == QLatin1String("user")) {
                    QJsonValue c = ev.value("message").toObject().value("content");
                    if (c.isString() && c.toString().contains(
                            QStringLiteral("<command-name>/audit</command-name>"))) {
                        pastAudit = true;
                    }
                }
                continue;
            }
            if (ev.value("type").toString() == QLatin1String("assistant")) {
                QString sr = ev.value("message").toObject()
                               .value("stop_reason").toString();
                if (sr == QLatin1String("end_turn")) {
                    auditFinished = true;
                    break;
                }
            }
        }
        if (auditFinished) inAudit = false;
    }
    if (inCompact) {
        newState = ClaudeState::Compacting;
        detail = QStringLiteral("compacting");
    }

    // Plan mode: most recent permission-mode event in the tail decides.
    // Claude Code records `{"type":"permission-mode","mode":"plan"}` when
    // the user toggles plan mode; switching out writes another with
    // mode == "default" / "acceptEdits". Search backward through the
    // events we already parsed (cheap — ~32KB tail).
    bool newPlan = false;
    for (int i = events.size() - 1; i >= 0; --i) {
        if (events[i].value("type").toString() != QLatin1String("permission-mode"))
            continue;
        const QString mode = events[i].value("mode").toString().toLower();
        newPlan = mode.contains(QLatin1String("plan"));
        break;
    }

    if (newPlan != m_planMode) {
        m_planMode = newPlan;
        emit planModeChanged(m_planMode);
    }
    if (inAudit != m_auditing) {
        m_auditing = inAudit;
        emit auditingChanged(m_auditing);
    }
    if (newState != m_state) {
        m_state = newState;
        emit stateChanged(m_state, detail);
    }
}

void ClaudeIntegration::updateChangedFiles(const QJsonObject &toolUse) {
    QString name = toolUse.value("name").toString();
    QJsonObject input = toolUse.value("input").toObject();

    QString filePath;
    if (name == "Edit" || name == "Write" || name == "Read") {
        filePath = input.value("file_path").toString();
    } else if (name == "Bash") {
        // Can't reliably extract file paths from bash commands
        return;
    }

    if (!filePath.isEmpty() && !m_changedFiles.contains(filePath)) {
        m_changedFiles.append(filePath);
        if (m_changedFiles.size() > 50)
            m_changedFiles.removeFirst();
        emit fileChanged(filePath);
    }
}

// --- Hook Server ---

bool ClaudeIntegration::startHookServer() {
    if (m_hookServer) return true;

    m_hookServer = new QLocalServer(this);
    QString socketPath = QDir::tempPath() + "/ants-claude-hooks-" +
                         QString::number(QCoreApplication::applicationPid());
    QLocalServer::removeServer(socketPath);

    if (!m_hookServer->listen(socketPath)) {
        delete m_hookServer;
        m_hookServer = nullptr;
        return false;
    }
    // Restrict socket permissions to owner only
    setOwnerOnlyPerms(socketPath);

    connect(m_hookServer, &QLocalServer::newConnection,
            this, &ClaudeIntegration::onHookConnection);
    return true;
}

void ClaudeIntegration::stopHookServer() {
    if (m_hookServer) {
        m_hookServer->close();
        delete m_hookServer;
        m_hookServer = nullptr;
    }
}


void ClaudeIntegration::onHookConnection() {
    while (m_hookServer->hasPendingConnections()) {
        QLocalSocket *socket = m_hookServer->nextPendingConnection();
        // Buffer incoming data — readyRead may fire with partial JSON
        socket->setProperty("_buf", QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [socket]() {
            QByteArray buf = socket->property("_buf").toByteArray();
            buf += socket->readAll();
            if (buf.size() > 10 * 1024 * 1024) { socket->disconnectFromServer(); return; }
            socket->setProperty("_buf", buf);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            QByteArray data = socket->property("_buf").toByteArray();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isObject())
                processHookEvent(doc.object());
            socket->deleteLater();
        });
    }
}

void ClaudeIntegration::processHookEvent(const QJsonObject &event) {
    QString hookName = event.value("hook_event_name").toString();
    QString toolName = event.value("tool_name").toString();
    QJsonObject toolInput = event.value("tool_input").toObject();

    if (hookName == "SessionStart") {
        m_activeSessionId = event.value("session_id").toString();
        m_state = ClaudeState::Idle;
        emit sessionStarted(m_activeSessionId);
        emit stateChanged(m_state, "session started");
    } else if (hookName == "PreToolUse") {
        m_state = ClaudeState::ToolUse;
        m_currentTool = toolName;
        emit toolStarted(toolName, toolInput.value("command").toString());
        emit stateChanged(m_state, toolName);
    } else if (hookName == "PostToolUse") {
        m_state = ClaudeState::Thinking;
        emit toolFinished(toolName, true);
        emit stateChanged(m_state, "thinking");
        updateChangedFiles(event);
    } else if (hookName == "PostToolUseFailure") {
        emit toolFinished(toolName, false);
    } else if (hookName == "Stop") {
        m_state = ClaudeState::Idle;
        m_currentTool.clear();
        QString reason = event.value("stop_reason").toString();
        emit sessionStopped(reason);
        emit stateChanged(m_state, "idle");
    } else if (hookName == "PermissionRequest") {
        QString input = toolInput.value("command").toString();
        if (input.isEmpty())
            input = toolInput.value("file_path").toString();
        emit permissionRequested(toolName, input);
    } else if (hookName == "PreCompact") {
        m_state = ClaudeState::Compacting;
        emit stateChanged(m_state, QStringLiteral("compacting"));
    }
}

// --- MCP Server ---

bool ClaudeIntegration::startMcpServer(const QString &socketPath) {
    if (m_mcpServer) return true;

    m_mcpServer = new QLocalServer(this);
    QLocalServer::removeServer(socketPath);

    if (!m_mcpServer->listen(socketPath)) {
        delete m_mcpServer;
        m_mcpServer = nullptr;
        return false;
    }
    // Restrict socket permissions to owner only
    setOwnerOnlyPerms(socketPath);

    connect(m_mcpServer, &QLocalServer::newConnection,
            this, &ClaudeIntegration::onMcpConnection);
    return true;
}

void ClaudeIntegration::stopMcpServer() {
    if (m_mcpServer) {
        m_mcpServer->close();
        delete m_mcpServer;
        m_mcpServer = nullptr;
    }
}

void ClaudeIntegration::setScrollbackProvider(std::function<QString(int)> provider) {
    m_scrollbackProvider = std::move(provider);
}

void ClaudeIntegration::setCwdProvider(std::function<QString()> provider) {
    m_cwdProvider = std::move(provider);
}

void ClaudeIntegration::setLastCommandProvider(std::function<QPair<int,QString>()> provider) {
    m_lastCommandProvider = std::move(provider);
}

void ClaudeIntegration::setGitStatusProvider(std::function<QString()> provider) {
    m_gitStatusProvider = std::move(provider);
}

void ClaudeIntegration::setEnvironmentProvider(std::function<QString()> provider) {
    m_envProvider = std::move(provider);
}

void ClaudeIntegration::onMcpConnection() {
    while (m_mcpServer->hasPendingConnections()) {
        QLocalSocket *socket = m_mcpServer->nextPendingConnection();
        // Buffer incoming data — readyRead may fire with partial JSON.
        // Try to parse on each readyRead; process once valid JSON is received.
        socket->setProperty("_buf", QByteArray());
        socket->setProperty("_handled", false);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            if (socket->property("_handled").toBool()) return;
            QByteArray buf = socket->property("_buf").toByteArray();
            buf += socket->readAll();
            if (buf.size() > 10 * 1024 * 1024) { socket->disconnectFromServer(); return; }
            socket->setProperty("_buf", buf);
            QJsonDocument doc = QJsonDocument::fromJson(buf);
            if (!doc.isObject()) return; // wait for more data
            socket->setProperty("_handled", true);

            QJsonObject request = doc.object();
            QString method = request.value("method").toString();
            QJsonValue reqId = request.value("id");

            // Build either `result` (success) or `error` (failure). The
            // envelope ({jsonrpc, id, result/error}) is added at the end.
            QJsonObject result;
            QJsonObject error;
            bool haveResult = false;

            if (method == "initialize") {
                QJsonObject caps;
                caps["tools"] = QJsonObject();
                QJsonObject serverInfo;
                serverInfo["name"] = "ants-terminal";
                serverInfo["version"] = QStringLiteral(ANTS_VERSION);
                result["protocolVersion"] = "2025-11-25";
                result["capabilities"] = caps;
                result["serverInfo"] = serverInfo;
                haveResult = true;
            } else if (method == "tools/list") {
                QJsonArray tools;

                QJsonObject scrollbackTool;
                scrollbackTool["name"] = "get_scrollback";
                scrollbackTool["description"] = "Get the last N lines of terminal scrollback";
                QJsonObject linesParam;
                linesParam["type"] = "integer";
                linesParam["default"] = 50;
                QJsonObject props;
                props["lines"] = linesParam;
                QJsonObject schema;
                schema["type"] = "object";
                schema["properties"] = props;
                scrollbackTool["inputSchema"] = schema;
                tools.append(scrollbackTool);

                QJsonObject cwdTool;
                cwdTool["name"] = "get_cwd";
                cwdTool["description"] = "Get the terminal's current working directory";
                tools.append(cwdTool);

                QJsonObject sessionTool;
                sessionTool["name"] = "get_session_info";
                sessionTool["description"] = "Get terminal session metadata";
                tools.append(sessionTool);

                QJsonObject lastCmdTool;
                lastCmdTool["name"] = "get_last_command";
                lastCmdTool["description"] = "Get the last command's exit code and output (via shell integration)";
                tools.append(lastCmdTool);

                QJsonObject gitTool;
                gitTool["name"] = "get_git_status";
                gitTool["description"] = "Get git branch, status, and recent commits for the terminal's CWD";
                tools.append(gitTool);

                QJsonObject envTool;
                envTool["name"] = "get_environment";
                envTool["description"] = "Get shell environment info (PATH, virtualenv, key env vars)";
                tools.append(envTool);

                result["tools"] = tools;
                haveResult = true;
            } else if (method == "tools/call") {
                QJsonObject params = request.value("params").toObject();
                QString toolName = params.value("name").toString();

                auto makeTextContent = [](const QString &text) {
                    QJsonObject block;
                    block["type"] = "text";
                    block["text"] = text;
                    QJsonArray arr;
                    arr.append(block);
                    return arr;
                };

                bool toolHandled = false;
                if (toolName == "get_scrollback" && m_scrollbackProvider) {
                    int lines = params.value("arguments").toObject()
                                     .value("lines").toInt(50);
                    result["content"] = makeTextContent(m_scrollbackProvider(lines));
                    toolHandled = true;
                } else if (toolName == "get_cwd" && m_cwdProvider) {
                    result["content"] = makeTextContent(m_cwdProvider());
                    toolHandled = true;
                } else if (toolName == "get_session_info") {
                    QJsonObject info;
                    info["state"] = static_cast<int>(m_state);
                    info["current_tool"] = m_currentTool;
                    info["context_percent"] = m_contextPercent;
                    info["changed_files"] = QJsonArray::fromStringList(m_changedFiles);
                    info["session_id"] = m_activeSessionId;
                    result["content"] = makeTextContent(
                        QJsonDocument(info).toJson(QJsonDocument::Compact));
                    toolHandled = true;
                } else if (toolName == "get_last_command" && m_lastCommandProvider) {
                    auto [exitCode, output] = m_lastCommandProvider();
                    QJsonObject info;
                    info["exit_code"] = exitCode;
                    info["output"] = output;
                    info["failed"] = (exitCode != 0);
                    result["content"] = makeTextContent(
                        QJsonDocument(info).toJson(QJsonDocument::Compact));
                    toolHandled = true;
                } else if (toolName == "get_git_status" && m_gitStatusProvider) {
                    result["content"] = makeTextContent(m_gitStatusProvider());
                    toolHandled = true;
                } else if (toolName == "get_environment" && m_envProvider) {
                    result["content"] = makeTextContent(m_envProvider());
                    toolHandled = true;
                }

                if (toolHandled) {
                    haveResult = true;
                } else {
                    // JSON-RPC application error: tool not found or provider missing.
                    error["code"] = -32602; // Invalid params
                    error["message"] = QString("Unknown tool: %1").arg(toolName);
                }
            } else {
                // JSON-RPC -32601 = Method not found
                error["code"] = -32601;
                error["message"] = QString("Method not found: %1").arg(method);
            }

            // Notifications (no id) must NOT receive a response per JSON-RPC 2.0.
            if (reqId.isUndefined() || reqId.isNull()) {
                socket->disconnectFromServer();
                return;
            }

            QJsonObject envelope;
            envelope["jsonrpc"] = "2.0";
            envelope["id"] = reqId;
            if (haveResult) envelope["result"] = result;
            else            envelope["error"]  = error;

            QByteArray resp = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
            socket->write(resp);
            socket->flush();
            socket->disconnectFromServer();
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

// --- Project / Session Discovery ---

QString ClaudeIntegration::decodeProjectPath(const QString &encoded) {
    // Claude Code encodes paths by replacing /, _, and spaces with dashes.
    // Decoding is ambiguous, so this is a best-effort fallback.
    // The real path should be extracted from transcript cwd fields instead.
    QString path = encoded;
    if (path.startsWith('-'))
        path = '/' + path.mid(1);
    path.replace('-', '/');
    return path;
}

QString ClaudeIntegration::encodeProjectPath(const QString &path) {
    // Matches Claude Code's encoding: replace / with -
    QString encoded = path;
    encoded.replace('/', '-');
    return encoded;
}

// Extract the real project path from a transcript's first user message cwd field
static QString extractCwdFromTranscript(const QString &transcriptPath) {
    QFile file(transcriptPath);
    if (!file.open(QIODevice::ReadOnly)) return {};

    int linesRead = 0;
    while (!file.atEnd() && linesRead < 30) {
        QByteArray line = file.readLine().trimmed();
        ++linesRead;
        if (line.isEmpty()) continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;
        QJsonObject obj = doc.object();
        // user messages and some other types carry cwd
        QString cwd = obj.value("cwd").toString();
        if (!cwd.isEmpty()) return cwd;
    }
    return {};
}

QList<ClaudeProject> ClaudeIntegration::discoverProjects() const {
    QList<ClaudeProject> projects;
    QString home = QDir::homePath();
    QDir projectsDir(home + "/.claude/projects");
    if (!projectsDir.exists()) return projects;

    // Load active session metadata to mark active sessions
    // Also build sessionId -> name + cwd map
    QSet<QString> activeSessionIds;
    QHash<QString, QString> sessionNames;   // sessionId -> name
    QHash<QString, QString> sessionCwds;    // sessionId -> cwd
    QDir sessionsDir(home + "/.claude/sessions");
    if (sessionsDir.exists()) {
        for (const QFileInfo &fi : sessionsDir.entryInfoList({"*.json"}, QDir::Files)) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (!doc.isObject()) continue;
            QJsonObject obj = doc.object();
            QString sid = obj.value("sessionId").toString();
            QString name = obj.value("name").toString();
            QString cwd = obj.value("cwd").toString();
            if (!name.isEmpty()) sessionNames[sid] = name;
            if (!cwd.isEmpty()) sessionCwds[sid] = cwd;
            // Check if the process is actually running
            int pid = obj.value("pid").toInt();
            if (pid > 0 && QFile::exists(QString("/proc/%1/cmdline").arg(pid)))
                activeSessionIds.insert(sid);
        }
    }

    for (const QString &dirName : projectsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir projDir(projectsDir.filePath(dirName));
        QFileInfoList jsonls = projDir.entryInfoList({"*.jsonl"}, QDir::Files, QDir::Time);
        if (jsonls.isEmpty()) continue;

        ClaudeProject project;
        project.encodedName = dirName;

        // Resolve the real project path from transcript or session metadata
        // Try session metadata first (most reliable), then transcript cwd
        QString realPath;
        for (const QFileInfo &fi : jsonls) {
            QString sid = fi.baseName();
            if (sessionCwds.contains(sid)) {
                realPath = sessionCwds[sid];
                break;
            }
        }
        if (realPath.isEmpty()) {
            // Fallback: extract from the most recent transcript
            realPath = extractCwdFromTranscript(jsonls.first().absoluteFilePath());
        }
        if (realPath.isEmpty()) {
            // Last resort: naive decode
            realPath = decodeProjectPath(dirName);
        }
        project.path = realPath;

        // Read project memory snippet
        project.memorySnippet = projectMemory(dirName);

        for (const QFileInfo &fi : jsonls) {
            ClaudeSession session;
            session.sessionId = fi.baseName();
            session.projectPath = project.path;
            session.projectEncoded = dirName;
            session.transcriptPath = fi.absoluteFilePath();
            session.lastModified = fi.lastModified();
            session.sizeBytes = fi.size();
            session.isActive = activeSessionIds.contains(session.sessionId);
            session.name = sessionNames.value(session.sessionId);

            // Lazy-load summary for the first few sessions per project
            if (project.sessions.size() < 5)
                session.firstMessage = sessionSummary(fi.absoluteFilePath());

            project.sessions.append(session);
        }

        if (!project.sessions.isEmpty())
            project.lastActivity = project.sessions.first().lastModified;

        projects.append(project);
    }

    // Sort projects by last activity (most recent first)
    std::sort(projects.begin(), projects.end(), [](const ClaudeProject &a, const ClaudeProject &b) {
        return a.lastActivity > b.lastActivity;
    });

    return projects;
}

QString ClaudeIntegration::sessionSummary(const QString &transcriptPath) const {
    QFile file(transcriptPath);
    if (!file.open(QIODevice::ReadOnly)) return {};

    // Read up to 50 lines to find the first user message
    int linesRead = 0;
    while (!file.atEnd() && linesRead < 50) {
        QByteArray line = file.readLine().trimmed();
        ++linesRead;
        if (line.isEmpty()) continue;

        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;
        QJsonObject obj = doc.object();

        if (obj.value("type").toString() != "user") continue;

        QJsonObject msg = obj.value("message").toObject();
        QJsonValue content = msg.value("content");

        QString text;
        if (content.isArray()) {
            for (const QJsonValue &c : content.toArray()) {
                QJsonObject block = c.toObject();
                if (block.value("type").toString() == "text") {
                    text = block.value("text").toString();
                    break;
                }
            }
        } else if (content.isString()) {
            text = content.toString();
        }

        if (!text.isEmpty()) {
            // Truncate to ~150 chars
            if (text.length() > 150)
                text = text.left(150) + "...";
            return text.simplified();
        }
    }
    return {};
}

QString ClaudeIntegration::projectMemory(const QString &projectEncoded) const {
    QString memPath = QDir::homePath() + "/.claude/projects/"
                      + projectEncoded + "/memory/MEMORY.md";
    QFile file(memPath);
    if (!file.open(QIODevice::ReadOnly)) return {};

    QString content = QString::fromUtf8(file.readAll());
    // Return first ~500 chars as snippet
    if (content.length() > 500)
        content = content.left(500) + "\n...";
    return content;
}

// --- Environment Setup ---

QProcessEnvironment ClaudeIntegration::claudeEnv() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("COLORTERM", "truecolor");
    env.insert("TERM", "xterm-256color");
    return env;
}

