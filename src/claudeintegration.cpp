#include "claudeintegration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QApplication>

ClaudeIntegration::ClaudeIntegration(QObject *parent) : QObject(parent) {
    // Poll for Claude Code process every 2 seconds
    m_pollTimer.setInterval(2000);
    connect(&m_pollTimer, &QTimer::timeout, this, &ClaudeIntegration::pollClaudeProcess);
}

ClaudeIntegration::~ClaudeIntegration() {
    stopHookServer();
    stopMcpServer();
}

// --- Process Detection ---

void ClaudeIntegration::setShellPid(pid_t pid) {
    m_shellPid = pid;
    if (pid > 0)
        m_pollTimer.start();
    else
        m_pollTimer.stop();
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

    for (pid_t pid : childPids) {
        QFile cmdFile(QString("/proc/%1/cmdline").arg(pid));
        if (!cmdFile.open(QIODevice::ReadOnly)) continue;
        QString cmdline = QString::fromUtf8(cmdFile.readAll()).replace('\0', ' ').toLower();
        cmdFile.close();

        if (cmdline.contains("claude") || cmdline.contains("claude-code")) {
            found = true;
            foundPid = pid;
            break;
        }
    }

    ClaudeState newState;
    if (!found) {
        newState = ClaudeState::NotRunning;
    } else if (m_claudePid == 0) {
        // Newly detected — start as Idle, transcript watcher will refine
        newState = ClaudeState::Idle;
    } else {
        // Already tracking — keep current state (Thinking/ToolUse) intact;
        // parseTranscriptForState() manages transitions via file watcher
        newState = m_state;
    }

    if (found && m_claudePid == 0) {
        m_claudePid = foundPid;
        // Try to find the active session transcript
        QString home = QDir::homePath();
        QDir claudeDir(home + "/.claude/projects");
        if (claudeDir.exists()) {
            // Find most recently modified transcript
            QFileInfoList transcripts;
            for (const QString &projDir : claudeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QDir proj(claudeDir.filePath(projDir));
                for (const QFileInfo &fi : proj.entryInfoList({"*.jsonl"}, QDir::Files, QDir::Time)) {
                    transcripts.append(fi);
                }
            }
            if (!transcripts.isEmpty()) {
                // Watch the most recent transcript
                QString transcriptPath = transcripts.first().absoluteFilePath();
                if (!m_transcriptWatcher.files().contains(transcriptPath)) {
                    m_transcriptWatcher.addPath(transcriptPath);
                    connect(&m_transcriptWatcher, &QFileSystemWatcher::fileChanged,
                            this, &ClaudeIntegration::parseTranscriptForState);
                }
            }
        }
    } else if (!found) {
        m_claudePid = 0;
    }

    if (newState != m_state) {
        m_state = newState;
        emit stateChanged(m_state, m_currentTool);
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

    // Read the last few lines of the transcript for current state
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;

    // Seek to near the end
    qint64 size = file.size();
    if (size > 8192) file.seek(size - 8192);

    QJsonObject lastEvent;
    while (!file.atEnd()) {
        QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) lastEvent = doc.object();
    }

    if (lastEvent.isEmpty()) return;

    QString type = lastEvent.value("type").toString();
    ClaudeState newState = m_state;
    QString detail;

    if (type == "assistant") {
        QJsonObject msg = lastEvent.value("message").toObject();
        QString stopReason = msg.value("stop_reason").toString();
        if (stopReason.isEmpty() || stopReason == "null") {
            newState = ClaudeState::Thinking;
            detail = "thinking";
        } else if (stopReason == "tool_use") {
            newState = ClaudeState::ToolUse;
            detail = "tool use";
        } else {
            // "end_turn", "max_tokens", etc. → waiting for user
            newState = ClaudeState::Idle;
            detail = "idle";
        }

        // Check for tool use in content
        QJsonArray content = msg.value("content").toArray();
        for (const QJsonValue &c : content) {
            QJsonObject block = c.toObject();
            if (block.value("type").toString() == "tool_use") {
                newState = ClaudeState::ToolUse;
                m_currentTool = block.value("name").toString();
                detail = m_currentTool;

                // Track file changes
                updateChangedFiles(block);
            }
        }
    } else if (type == "result") {
        // Tool result received — Claude is now thinking about the result
        newState = ClaudeState::Thinking;
        detail = "thinking";
    } else if (type == "human" || type == "user") {
        // User sent a message — Claude will start thinking
        newState = ClaudeState::Thinking;
        detail = "thinking";
    }

    // Estimate context usage from token counts
    QJsonObject usage = lastEvent.value("message").toObject().value("usage").toObject();
    int inputTokens = usage.value("input_tokens").toInt();
    if (inputTokens > 0) {
        // Rough estimate: 200K context window
        m_contextPercent = std::min(100, inputTokens * 100 / 200000);
        emit contextUpdated(m_contextPercent);
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
                         QString::number(QApplication::applicationPid());
    QLocalServer::removeServer(socketPath);

    if (!m_hookServer->listen(socketPath)) {
        delete m_hookServer;
        m_hookServer = nullptr;
        return false;
    }

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

int ClaudeIntegration::hookServerPort() const {
    return m_hookServer ? 1 : 0; // Unix socket, not TCP port
}

void ClaudeIntegration::onHookConnection() {
    while (m_hookServer->hasPendingConnections()) {
        QLocalSocket *socket = m_hookServer->nextPendingConnection();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            QByteArray data = socket->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isObject())
                processHookEvent(doc.object());
            socket->deleteLater();
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
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
        emit stateChanged(ClaudeState::Thinking, "compacting context");
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
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            QByteArray data = socket->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (!doc.isObject()) { socket->deleteLater(); return; }

            QJsonObject request = doc.object();
            QString method = request.value("method").toString();
            QJsonObject response;

            if (method == "tools/list") {
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

                response["tools"] = tools;
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

                if (toolName == "get_scrollback" && m_scrollbackProvider) {
                    int lines = params.value("arguments").toObject()
                                     .value("lines").toInt(50);
                    response["content"] = makeTextContent(m_scrollbackProvider(lines));
                } else if (toolName == "get_cwd" && m_cwdProvider) {
                    response["content"] = makeTextContent(m_cwdProvider());
                } else if (toolName == "get_session_info") {
                    QJsonObject info;
                    info["state"] = static_cast<int>(m_state);
                    info["current_tool"] = m_currentTool;
                    info["context_percent"] = m_contextPercent;
                    info["changed_files"] = QJsonArray::fromStringList(m_changedFiles);
                    info["session_id"] = m_activeSessionId;
                    response["content"] = makeTextContent(
                        QJsonDocument(info).toJson(QJsonDocument::Compact));
                } else if (toolName == "get_last_command" && m_lastCommandProvider) {
                    auto [exitCode, output] = m_lastCommandProvider();
                    QJsonObject info;
                    info["exit_code"] = exitCode;
                    info["output"] = output;
                    info["failed"] = (exitCode != 0);
                    response["content"] = makeTextContent(
                        QJsonDocument(info).toJson(QJsonDocument::Compact));
                } else if (toolName == "get_git_status" && m_gitStatusProvider) {
                    response["content"] = makeTextContent(m_gitStatusProvider());
                } else if (toolName == "get_environment" && m_envProvider) {
                    response["content"] = makeTextContent(m_envProvider());
                }
            }

            // Send response
            QByteArray resp = QJsonDocument(response).toJson(QJsonDocument::Compact);
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

// --- Auto-configure Hooks ---

void ClaudeIntegration::ensureHooksConfigured(const QString &projectDir, int port) {
    Q_UNUSED(port);
    QString settingsPath = projectDir + "/.claude/settings.local.json";

    // Read existing settings
    QJsonObject root;
    QFile file(settingsPath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) root = doc.object();
        file.close();
    }

    // Check if hooks already configured
    QJsonObject hooks = root.value("hooks").toObject();
    if (hooks.contains("PreToolUse") || hooks.contains("Stop"))
        return; // Already configured

    // Add hook configuration for Ants Terminal notifications
    // Uses a simple file-based approach: write event to a temp file
    QString hookCmd = QString(
        "jq -c '{hook_event_name: .hook_event_name, tool_name: .tool_name, "
        "tool_input: .tool_input, session_id: .session_id}' "
        "> /tmp/ants-claude-event-%1.json"
    ).arg(QApplication::applicationPid());

    auto makeHook = [&hookCmd]() {
        return QJsonArray{QJsonObject{
            {"hooks", QJsonArray{QJsonObject{
                {"type", "command"},
                {"command", hookCmd}
            }}}
        }};
    };

    hooks["PreToolUse"] = makeHook();
    hooks["PostToolUse"] = makeHook();
    hooks["Stop"] = makeHook();
    hooks["SessionStart"] = makeHook();
    root["hooks"] = hooks;

    // Write settings
    QDir().mkpath(projectDir + "/.claude");
    QSaveFile saveFile(settingsPath);
    if (saveFile.open(QIODevice::WriteOnly)) {
        saveFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        saveFile.commit();
    }
}
