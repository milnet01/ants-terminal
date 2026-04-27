#include "claudetabtracker.h"

#include "configpaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

ClaudeTabTracker::ClaudeTabTracker(QObject *parent) : QObject(parent) {
    // 2 s matches ClaudeIntegration's poll cadence. We could run faster for
    // snappier claude-starts detection, but the transcript watcher is the
    // real event source — polling only catches the start/stop edges.
    m_pollTimer.setInterval(2000);
    connect(&m_pollTimer, &QTimer::timeout, this, &ClaudeTabTracker::pollAllShells);

    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, [this](const QString &path) {
        auto it = m_pathToShell.constFind(path);
        if (it == m_pathToShell.constEnd()) return;
        reparseTranscript(it.value());
    });
}

ClaudeTabTracker::~ClaudeTabTracker() = default;

void ClaudeTabTracker::trackShell(pid_t shellPid) {
    if (shellPid <= 0) return;

    ShellEntry &entry = m_shells[shellPid];
    const ShellState before = entry.state;

    if (entry.shellPid == 0) {
        entry.shellPid = shellPid;
    }

    if (!m_pollTimer.isActive()) m_pollTimer.start();

    // Immediate probe so a tab that opens mid-Claude-session picks up state
    // without waiting for the first 2 s tick.
    detectClaudeChild(entry);
    if (!entry.transcriptPath.isEmpty())
        reparseTranscript(shellPid);
    maybeEmit(shellPid, before);
}

void ClaudeTabTracker::untrackShell(pid_t shellPid) {
    auto it = m_shells.find(shellPid);
    if (it == m_shells.end()) return;

    if (!it->transcriptPath.isEmpty())
        releaseWatch(it->transcriptPath);

    m_shells.erase(it);

    if (m_shells.isEmpty()) m_pollTimer.stop();
}

void ClaudeTabTracker::markShellAwaitingInput(pid_t shellPid, bool awaiting) {
    auto it = m_shells.find(shellPid);
    if (it == m_shells.end()) {
        // Shell isn't tracked yet — create a minimal entry so the flag
        // survives until trackShell lands. (Callers may flag permission-
        // prompt state the instant the hook fires, before the shell is
        // otherwise wired through.)
        if (!awaiting) return;
        ShellEntry &entry = m_shells[shellPid];
        entry.shellPid = shellPid;
        const ShellState before = entry.state;
        entry.state.awaitingInput = true;
        maybeEmit(shellPid, before);
        return;
    }
    if (it->state.awaitingInput == awaiting) return;
    const ShellState before = it->state;
    it->state.awaitingInput = awaiting;
    maybeEmit(shellPid, before);
}

ClaudeTabTracker::ShellState ClaudeTabTracker::shellState(pid_t shellPid) const {
    auto it = m_shells.constFind(shellPid);
    return it == m_shells.constEnd() ? ShellState{} : it->state;
}

pid_t ClaudeTabTracker::shellForSessionId(const QString &sessionId) const {
    if (sessionId.isEmpty()) return 0;
    // Claude Code stores transcripts as <session-uuid>.jsonl — compare on
    // basename minus the .jsonl suffix. Avoids parsing the JSONL body
    // (which would add overhead on every permission prompt) while still
    // being exact against the filename the process writes to.
    for (auto it = m_shells.constBegin(); it != m_shells.constEnd(); ++it) {
        if (it->transcriptPath.isEmpty()) continue;
        const int slash = it->transcriptPath.lastIndexOf('/');
        const int dot = it->transcriptPath.lastIndexOf('.');
        const int start = slash + 1;
        const int end = (dot > slash) ? dot : it->transcriptPath.size();
        const QStringView basename =
            QStringView(it->transcriptPath).mid(start, end - start);
        if (basename == sessionId) return it.key();
    }
    return 0;
}

void ClaudeTabTracker::forceRefreshForTest(pid_t shellPid,
                                           const QString &transcriptPath) {
    if (!transcriptPath.isEmpty()) {
        // Seed the entry for this pid with the injected transcript path.
        // Bypasses the proc-walk (which needs a real Claude child under
        // /proc/shellPid/children) by setting claudePid to a sentinel so
        // detectClaudeChild sees the shell as "has Claude" and doesn't
        // wipe the path out on the next poll.
        ShellEntry &entry = m_shells[shellPid];
        if (entry.shellPid == 0) entry.shellPid = shellPid;
        if (entry.claudePid == 0) entry.claudePid = -1;  // sentinel
        if (entry.transcriptPath != transcriptPath) {
            if (!entry.transcriptPath.isEmpty()) releaseWatch(entry.transcriptPath);
            entry.transcriptPath = transcriptPath;
            m_pathToShell.insert(transcriptPath, shellPid);
        }
    }
    auto it = m_shells.find(shellPid);
    if (it == m_shells.end()) return;
    if (it->transcriptPath.isEmpty()) return;
    reparseTranscript(shellPid);
}

void ClaudeTabTracker::pollAllShells() {
    const QList<pid_t> pids = m_shells.keys();
    for (pid_t pid : pids) {
        auto it = m_shells.find(pid);
        if (it == m_shells.end()) continue;
        const ShellState before = it->state;
        detectClaudeChild(*it);
        maybeEmit(pid, before);
    }
}

void ClaudeTabTracker::detectClaudeChild(ShellEntry &entry) {
    if (entry.shellPid <= 0) return;

    QFile childFile(QString("/proc/%1/task/%1/children").arg(entry.shellPid));
    QList<pid_t> childPids;
    if (childFile.open(QIODevice::ReadOnly)) {
        QString children = QString::fromUtf8(childFile.readAll()).trimmed();
        childFile.close();
        for (const QString &pidStr : children.split(' ', Qt::SkipEmptyParts)) {
            bool ok;
            pid_t pid = pidStr.toInt(&ok);
            if (ok && pid > 0) childPids.append(pid);
        }
    }

    auto basename = [](const QString &path) -> QString {
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? path.mid(slash + 1) : path;
    };
    auto isClaudeBin = [](const QString &name) {
        return name == QLatin1String("claude") || name == QLatin1String("claude-code");
    };

    pid_t found = 0;
    for (pid_t pid : childPids) {
        QFile cmdFile(QString("/proc/%1/cmdline").arg(pid));
        if (!cmdFile.open(QIODevice::ReadOnly)) continue;
        QByteArray raw = cmdFile.readAll();
        cmdFile.close();
        QList<QByteArray> argv = raw.split('\0');
        while (!argv.isEmpty() && argv.last().isEmpty()) argv.removeLast();
        if (argv.isEmpty()) continue;

        QString arg0 = basename(QString::fromUtf8(argv.first()));
        bool match = isClaudeBin(arg0);
        if (!match && (arg0 == QLatin1String("node") ||
                       arg0 == QLatin1String("deno") ||
                       arg0 == QLatin1String("bun"))) {
            for (qsizetype i = 1; i < argv.size(); ++i) {
                QString scriptName = basename(QString::fromUtf8(argv[i]));
                QString full = QString::fromUtf8(argv[i]);
                if (isClaudeBin(scriptName) ||
                    full.contains(QLatin1String("/claude-code/")) ||
                    full.contains(QLatin1String("/claude/"))) {
                    match = true;
                    break;
                }
            }
        }
        if (match) { found = pid; break; }
    }

    if (found == 0) {
        if (entry.claudePid != 0) {
            if (!entry.transcriptPath.isEmpty()) {
                releaseWatch(entry.transcriptPath);
                entry.transcriptPath.clear();
            }
            entry.claudePid = 0;
        }
        entry.state.state = ClaudeState::NotRunning;
        entry.state.tool.clear();
        // Plan-mode latch doesn't survive Claude exiting — next session
        // starts fresh.
        entry.state.planMode = false;
        return;
    }

    if (entry.claudePid == 0) {
        entry.claudePid = found;
        QDir claudeDir(ConfigPaths::claudeProjectsDir());
        if (claudeDir.exists()) {
            QFileInfo newest;
            for (const QString &projDir : claudeDir.entryList(
                    QDir::Dirs | QDir::NoDotAndDotDot)) {
                QDir proj(claudeDir.filePath(projDir));
                for (const QFileInfo &fi : proj.entryInfoList(
                        {"*.jsonl"}, QDir::Files)) {
                    if (!newest.exists() || fi.lastModified() > newest.lastModified())
                        newest = fi;
                }
            }
            if (newest.exists()) {
                entry.transcriptPath = newest.absoluteFilePath();
                m_watcher.addPath(entry.transcriptPath);
                m_pathToShell.insert(entry.transcriptPath, entry.shellPid);
            }
        }
        entry.state.state = ClaudeState::Idle;
    }
}

void ClaudeTabTracker::reparseTranscript(pid_t shellPid) {
    auto it = m_shells.find(shellPid);
    if (it == m_shells.end()) return;
    if (it->transcriptPath.isEmpty()) return;

    // Re-add the watch — QFileSystemWatcher drops it after atomic saves.
    if (!m_watcher.files().contains(it->transcriptPath)) {
        m_watcher.addPath(it->transcriptPath);
        m_pathToShell.insert(it->transcriptPath, shellPid);
    }

    const ShellState before = it->state;
    const ClaudeTranscriptSnapshot snap =
        ClaudeIntegration::parseTranscriptTail(it->transcriptPath, it->state.planMode);
    if (!snap.hasEvents) return;

    if (snap.stateDetermined) {
        it->state.state = snap.state;
        it->state.tool = snap.tool;
    }
    it->state.planMode = snap.planMode;
    it->state.auditing = snap.auditing;

    maybeEmit(shellPid, before);
}

void ClaudeTabTracker::releaseWatch(const QString &path) {
    if (m_watcher.files().contains(path))
        m_watcher.removePath(path);
    m_pathToShell.remove(path);
}

void ClaudeTabTracker::maybeEmit(pid_t shellPid, const ShellState &before) {
    auto it = m_shells.constFind(shellPid);
    if (it == m_shells.constEnd()) return;
    const ShellState &after = it->state;
    if (before.state == after.state &&
        before.tool == after.tool &&
        before.planMode == after.planMode &&
        before.awaitingInput == after.awaitingInput &&
        before.auditing == after.auditing) {
        return;
    }
    emit shellStateChanged(shellPid);
}
