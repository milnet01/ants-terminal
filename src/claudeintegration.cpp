#include "claudeintegration.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QSaveFile>
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

    // Scan /proc for claude processes that are children of our shell
    QDir procDir("/proc");
    bool found = false;
    pid_t foundPid = 0;

    for (const QString &entry : procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok;
        pid_t pid = entry.toInt(&ok);
        if (!ok || pid <= 0) continue;

        // Check if this process is a child of our shell
        QFile statFile(QString("/proc/%1/stat").arg(pid));
        if (!statFile.open(QIODevice::ReadOnly)) continue;
        QString stat = QString::fromUtf8(statFile.readAll());
        statFile.close();

        // Parse ppid (4th field, after comm which may contain spaces)
        int closeParenIdx = stat.lastIndexOf(')');
        if (closeParenIdx < 0) continue;
        QStringList fields = stat.mid(closeParenIdx + 2).split(' ');
        if (fields.size() < 2) continue;
        pid_t ppid = fields[1].toInt();

        if (ppid != m_shellPid) continue;

        // Check if this is Claude Code by reading the command line
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

    ClaudeState newState = found ? ClaudeState::Idle : ClaudeState::NotRunning;

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
    // Read the last few lines of the transcript for current state
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;

    // Seek to near the end
    qint64 size = file.size();
    if (size > 4096) file.seek(size - 4096);

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
        } else {
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
