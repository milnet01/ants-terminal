#include "claudeintegration.h"

#include "configpaths.h"
#include "debuglog.h"
#include "secureio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QCoreApplication>

#include <sys/socket.h>
#include <unistd.h>

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

void ClaudeIntegration::forgetShell(pid_t pid) {
    // ANTS-1131 — drop the per-PID plan-mode cache entry. Called
    // from MainWindow::closeTab alongside untrackShell so the cache
    // doesn't accumulate stale entries over a long session and so
    // Linux PID reuse doesn't poison a fresh shell with a stale
    // flag from a closed Claude tab.
    if (pid > 0) m_planModeByPid.remove(pid);
}

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
        // Cache the outgoing tab's plan-mode state so we can restore
        // it on a future tab-switch back. Without the cache, returning
        // to a tab whose Claude session is in plan mode but whose
        // transcript-tail window doesn't include the permission-mode
        // event would silently drop the "plan mode" indicator.
        // 0.7.54 (2026-04-27 indie-review).
        if (m_shellPid > 0) m_planModeByPid[m_shellPid] = m_planMode;

        m_state = ClaudeState::NotRunning;
        m_currentTool.clear();
        m_contextPercent = 0;
        m_claudePid = 0;            // force pollClaudeProcess to re-detect
        m_activeSessionId.clear();

        // Restore the incoming pid's cached plan mode if we have one.
        // pollClaudeProcess will re-derive from transcript anyway, but
        // restoring the cache first means the indicator doesn't flicker
        // off→on across the tab switch.
        const bool cached = pid > 0 && m_planModeByPid.value(pid, false);
        if (m_planMode != cached) {
            m_planMode = cached;
            emit planModeChanged(cached);
        }
        if (m_auditing) { m_auditing = false; emit auditingChanged(false); }
        if (!m_transcriptPath.isEmpty()) {
            m_transcriptWatcher.removePath(m_transcriptPath);
            m_transcriptPath.clear();
        }
        // ANTS-1168: m_changedFiles is per-tab/per-session; clearing it
        // on PID change keeps MCP `get_session_info` from returning the
        // prior tab's edited-file list to the new tab's queries.
        m_changedFiles.clear();
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

// Shared with ClaudeTabTracker. /proc-walking moved here in 0.7.57
// (ANTS-1048) — see header for context.
pid_t ClaudeIntegration::findClaudeChildPid(pid_t shellPid) {
    if (shellPid <= 0) return 0;

    // Read child PIDs from the kernel's children file (much faster than
    // scanning all /proc).
    QList<pid_t> childPids;
    QFile childFile(QString("/proc/%1/task/%1/children").arg(shellPid));
    if (childFile.open(QIODevice::ReadOnly)) {
        QString children = QString::fromUtf8(childFile.readAll()).trimmed();
        childFile.close();
        for (const QString &pidStr : children.split(' ', Qt::SkipEmptyParts)) {
            bool ok;
            pid_t pid = pidStr.toInt(&ok);
            if (ok && pid > 0) childPids.append(pid);
        }
    } else {
        // Fallback: scan /proc but only check stat for ppid match.
        // Resilience for kernels / containers that don't expose
        // /proc/<pid>/task/<pid>/children.
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
            if (ppid == shellPid) childPids.append(pid);
        }
    }

    // Match the executable, not any substring. "grep claude file" or a
    // user with "~/bin/claude-search" must NOT be mistaken for Claude Code.
    auto basename = [](const QString &path) -> QString {
        int slash = path.lastIndexOf('/');
        return slash >= 0 ? path.mid(slash + 1) : path;
    };
    auto isClaudeBin = [](const QString &name) {
        return name == QLatin1String("claude") ||
               name == QLatin1String("claude-code");
    };

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

        // Node/deno/bun launchers: inspect argv[1..] for a script basename
        // or a path containing "/claude/" or "/claude-code/".
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

        if (match) return pid;
    }
    return 0;
}

void ClaudeIntegration::pollClaudeProcess() {
    // ANTS-1225-INV-4: gate the whole function on a valid focused-tab
    // shell PID. Without this, findClaudeChildPid(0) would walk
    // /proc/0/task/0/children which is meaningless.
    if (m_shellPid <= 0) return;

    const pid_t foundPid = findClaudeChildPid(m_shellPid);
    const bool found = foundPid > 0;

    if (!found) {
        // ANTS-1225-INV-3: when no Claude is running under the focused
        // shell, clear m_claudePid + m_transcriptPath and announce
        // NotRunning. This branch coexists with INV-1 — the != gate
        // below does NOT subsume the not-found case.
        m_claudePid = 0;
        m_transcriptPath.clear();
        // Indie-review 2026-05-13: prune the plan-mode cache for this
        // shell-pid. Linux PID reuse is fast; a tab that crashed before
        // closeTab ran would otherwise poison a future tab assigned the
        // same shell pid with the dead tab's plan-mode flag.
        m_planModeByPid.remove(m_shellPid);
        if (m_state != ClaudeState::NotRunning) {
            m_state = ClaudeState::NotRunning;
            emit stateChanged(m_state, m_currentTool);
        }
        return;
    }

    // ANTS-1225-INV-1: rebind on either initial detection (m_claudePid==0)
    // OR live PID replacement (m_claudePid set to a stale dead PID after
    // /exit + claude --resume completed within the 2 s poll window).
    // Pre-1225 this gate was `m_claudePid == 0`, missing the replacement
    // case — the status indicator would stay hidden until tab-switch
    // because setShellPid (line 88) zeroes m_claudePid as a side effect.
    if (m_claudePid != foundPid) {
        // Newly detected Claude process (or replacement of a dead one).
        m_claudePid = foundPid;

        // ANTS-1168: scope to the focused tab's project rather than
        // walking ALL ~/.claude/projects entries. Without scoping, the
        // tab whose Claude process JUST started momentarily reads the
        // transcript for whichever other project happened to be most
        // recently active globally — same shape as ANTS-1163's stale-
        // session bug, which fixed `sessionPathForCwd` but not this
        // second site.
        const QFileInfo cwdInfo(QString("/proc/%1/cwd").arg(m_shellPid));
        const QString projectCwd =
            cwdInfo.exists() ? cwdInfo.symLinkTarget() : QString();
        const qint64 procStartMs = processStartTimeMs(m_claudePid);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QString scoped =
            sessionPathForCwd(projectCwd, procStartMs, nowMs);

        if (!scoped.isEmpty()) {
            // ANTS-1225-INV-2: rebind sequence — watcher swap, then
            // seed state from the new transcript tail. Order matters:
            // addPath before parseTranscriptForState so the watcher is
            // armed before the first parse.
            m_transcriptPath = scoped;

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

        // Set initial Idle state, then let transcript parse refine it.
        // ANTS-1225-INV-2: the m_state != Idle guard is mandatory —
        // removing it makes the status indicator flap on every poll
        // when steady-state Idle.
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

// ANTS-1163: read the most recent ISO 8601 `timestamp` field from the
// JSONL tail. Walks backwards over the last 32 KB so we don't pay the
// full transcript scan on every refresh tick. Returns 0 when no
// timestamped event sits in the tail window — caller falls back to
// file mtime.
qint64 ClaudeIntegration::lastEventTimestampMs(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;

    constexpr qint64 kWindow = 32 * 1024;
    const qint64 size = f.size();
    const qint64 start = std::max<qint64>(0, size - kWindow);
    if (!f.seek(start)) return 0;
    QByteArray tail = f.read(size - start);
    f.close();

    // Drop a likely-truncated leading partial line if we didn't read
    // from offset 0 — same trick parseTranscriptTail uses.
    if (start > 0) {
        const int firstNl = tail.indexOf('\n');
        if (firstNl < 0) return 0;
        tail.remove(0, firstNl + 1);
    }

    const QList<QByteArray> lines = tail.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray line = it->trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;
        const QJsonValue ts = doc.object().value(QStringLiteral("timestamp"));
        if (!ts.isString()) continue;
        const QDateTime dt = QDateTime::fromString(
            ts.toString(), Qt::ISODateWithMs);
        if (!dt.isValid()) continue;
        return dt.toMSecsSinceEpoch();
    }
    return 0;
}

// ANTS-1163: wall-clock epoch ms when `pid` started. /proc/<pid>/stat
// field 22 is starttime in clock ticks since boot; /proc/stat's
// `btime` line is boot time epoch seconds. Their sum is the process
// start in epoch seconds; multiply by 1000 for ms. Returns 0 on any
// parse failure (treated as "no anchor available").
qint64 ClaudeIntegration::processStartTimeMs(pid_t pid) {
    if (pid <= 0) return 0;

    QFile statF(QStringLiteral("/proc/%1/stat").arg(static_cast<int>(pid)));
    if (!statF.open(QIODevice::ReadOnly)) return 0;
    const QByteArray statRaw = statF.readAll();
    statF.close();

    // The `comm` field (parenthesized) can contain spaces/parens. The
    // safe parse: scan after the closing `)` of comm, then split the
    // remainder by space — field 22 is at index 19 (state is 0 in the
    // post-comm slice; starttime is the 22nd field overall, indexed
    // from 1; in the post-comm slice it's index 19).
    const int closeParen = statRaw.lastIndexOf(')');
    if (closeParen < 0) return 0;
    const QByteArray rest = statRaw.mid(closeParen + 1).trimmed();
    const QList<QByteArray> fields = rest.split(' ');
    if (fields.size() < 20) return 0;
    bool ok = false;
    const qulonglong starttimeTicks = fields[19].toULongLong(&ok);
    if (!ok) return 0;

    // /proc/stat reports `size() == 0` so QFile::atEnd() / readLine()
    // loops bail out immediately. readAll() is the only reliable way
    // to get the full body of a /proc text file.
    QFile bootF(QStringLiteral("/proc/stat"));
    if (!bootF.open(QIODevice::ReadOnly)) return 0;
    const QByteArray bootRaw = bootF.readAll();
    bootF.close();
    qulonglong btime = 0;
    for (const QByteArray &line : bootRaw.split('\n')) {
        if (!line.startsWith("btime ")) continue;
        btime = line.mid(6).trimmed().toULongLong(&ok);
        if (!ok) btime = 0;
        break;
    }
    if (btime == 0) return 0;

    const long ticksPerSec = ::sysconf(_SC_CLK_TCK);
    if (ticksPerSec <= 0) return 0;
    const qulonglong startSec = btime + (starttimeTicks / ticksPerSec);
    return static_cast<qint64>(startSec) * 1000;
}

// Effective last-event ms for a JSONL — last timestamped event in the
// tail if found, else file mtime. Used as the freshness signal in
// sessionPathForCwd.
static qint64 effectiveLastEventMs(const QFileInfo &fi) {
    const qint64 fromContent =
        ClaudeIntegration::lastEventTimestampMs(fi.absoluteFilePath());
    if (fromContent > 0) return fromContent;
    return fi.lastModified().toMSecsSinceEpoch();
}

QString ClaudeIntegration::sessionPathForCwd(const QString &projectCwd,
                                              qint64 minLastEventMs,
                                              qint64 nowMs) {
    if (projectCwd.isEmpty()) return {};
    QDir claudeDir(ConfigPaths::claudeProjectsDir());
    if (!claudeDir.exists()) return {};

    // ANTS-1163: clock-skew leeway. Claude Code may write its first
    // event a few ms before /proc/<pid>/stat reports the process
    // started (rare; clock-tick rounding plus our ms-conversion).
    constexpr qint64 kLeewayMs = 5'000;
    // 24 h floor when filter (a) is active (Claude PID known) — this
    // is a wide safety net rejecting truly ancient transcripts.
    constexpr qint64 kStaleMaxMsWithPid = 24LL * 60 * 60 * 1000;
    // ANTS-1163 follow-up (2026-05-08): tight floor when filter (a)
    // is INACTIVE (m_claudePid==0, cold start before pollClaudeProcess
    // detects the new Claude process). User repro: relaunched Ants +
    // Claude Code, opened Task List dialog, saw 27 done tasks from
    // yesterday's session — within 24 h, so the wide floor let them
    // through. The window where m_claudePid is briefly 0 is 1-3
    // seconds; 5 minutes is generous leeway. Anything older than that
    // and m_claudePid is still 0 means "no live Claude process," in
    // which case prior-session tasks should NOT surface.
    constexpr qint64 kStaleMaxMsNoPid = 5LL * 60 * 1000;

    // Project-scoped walk: encode each ancestor of `projectCwd` and
    // probe `~/.claude/projects/<encoded>/`. Deepest match wins —
    // catches the case where Claude Code was launched from a
    // sub-directory of the visible project root, and the inverse.
    QDir cur(projectCwd);
    while (true) {
        const QString encoded = encodeProjectPath(cur.absolutePath());
        QDir proj(claudeDir.filePath(encoded));
        if (proj.exists()) {
            QFileInfo bestInfo;
            qint64 bestEffMs = 0;
            for (const QFileInfo &fi : proj.entryInfoList({"*.jsonl"}, QDir::Files, QDir::Time)) {
                const qint64 effMs = effectiveLastEventMs(fi);
                // Process-anchored identity filter (a).
                if (minLastEventMs > 0 && effMs < minLastEventMs - kLeewayMs)
                    continue;
                // Liveness floor (b). Tight when (a) is inactive
                // (no PID known); wide otherwise.
                if (nowMs > 0) {
                    const qint64 floor = (minLastEventMs > 0)
                                             ? kStaleMaxMsWithPid
                                             : kStaleMaxMsNoPid;
                    if (effMs < nowMs - floor) continue;
                }
                if (effMs > bestEffMs) {
                    bestEffMs = effMs;
                    bestInfo = fi;
                }
            }
            if (bestInfo.exists()) return bestInfo.absoluteFilePath();
            // A matching project dir exists but no candidate survived
            // the freshness filter — return empty rather than walking
            // up to a parent dir whose JSONLs would also be stale.
            if (minLastEventMs > 0 || nowMs > 0) return {};
        }
        if (!cur.cdUp()) break;
    }
    // No match for this project tree — return empty rather than
    // leaking another project's transcript into the per-tab
    // surfaces (background tasks, etc.).
    return {};
}

QString ClaudeIntegration::activeSessionPath(const QString &projectCwd) const {
    if (!projectCwd.isEmpty()) {
        // ANTS-1163: thread the process-start anchor (a) and the 24 h
        // liveness floor (b). When m_claudePid is 0 (claude not yet
        // detected, or not running), the process anchor degrades to
        // 0 — disabled — and only the liveness floor applies. That
        // still rejects week-old transcripts, which is the failure
        // mode the user reported on cold start.
        const qint64 procStartMs = processStartTimeMs(m_claudePid);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        return sessionPathForCwd(projectCwd, procStartMs, nowMs);
    }

    // Unscoped fallback — system-wide newest .jsonl. Legacy callers.
    QDir claudeDir(ConfigPaths::claudeProjectsDir());
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
    // Skip excessively large transcripts to prevent memory exhaustion.
    // Surface the cap-hit so a "transcript empty / status frozen"
    // bug report is debuggable from logs (env-gated to avoid spam).
    constexpr qint64 kMaxTranscriptBytes = 100ll * 1024 * 1024;
    if (file.size() > kMaxTranscriptBytes) {
        ANTS_LOG(DebugLog::Claude,
                 "loadTranscript: %s exceeds %lld-byte cap (size=%lld); "
                 "returning empty",
                 path.toUtf8().constData(),
                 static_cast<long long>(kMaxTranscriptBytes),
                 static_cast<long long>(file.size()));
        return entries;
    }

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
    QDir claudeDir(ConfigPaths::claudeProjectsDir());
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

QStringList ClaudeIntegration::recentSessionsForCwd(const QString &projectCwd) const {
    if (projectCwd.isEmpty()) return recentSessions();

    QStringList sessions;
    QDir claudeDir(ConfigPaths::claudeProjectsDir());
    if (!claudeDir.exists()) return sessions;

    // Walk from projectCwd up the directory tree, taking the first
    // ancestor that has an encoded project entry on disk (mirrors
    // sessionPathForCwd's deepest-match-wins logic).
    QDir cur(projectCwd);
    while (true) {
        const QString encoded = encodeProjectPath(cur.absolutePath());
        QDir proj(claudeDir.filePath(encoded));
        if (proj.exists()) {
            for (const QFileInfo &fi : proj.entryInfoList({"*.jsonl"},
                                                          QDir::Files, QDir::Time)) {
                sessions.append(fi.absoluteFilePath());
                if (sessions.size() >= 20) return sessions;
            }
            return sessions;
        }
        if (!cur.cdUp()) break;
    }
    return sessions;
}

ClaudeTranscriptSnapshot ClaudeIntegration::parseTranscriptTail(
        const QString &path, bool latchedPlanMode) {
    ClaudeTranscriptSnapshot snap;
    snap.planMode = latchedPlanMode;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return snap;

    // Read a tail window large enough to span the last few turns. Claude Code
    // writes several metadata-only events at the end of each turn
    // (`system/turn_duration`, `last-prompt`, `permission-mode`,
    // `file-history-snapshot`, `summary`), so we need enough buffer to walk
    // back past them to the real state-determining event
    // (`assistant`/`user`/`attachment`).
    //
    // A single tool_result event carrying inline file contents routinely
    // exceeds 32 KB. If the starting window lands inside one such event, the
    // old code treated the whole buffer as "first line (likely truncated)"
    // and discarded everything — losing the very events that drive state.
    //
    // Grow the window (doubling, capped) until either (a) we've read the
    // whole file, or (b) the buffer contains at least two newlines so that
    // trimming up to and including the first newline (which marks the end
    // of a potentially-truncated prefix line) still leaves real content
    // behind for the parser.
    const qint64 size = file.size();
    qint64 window = 32768;
    constexpr qint64 kMaxWindow = 4 * 1024 * 1024; // 4 MiB safety cap
    QByteArray tail;
    while (true) {
        const qint64 start = std::max(qint64(0), size - window);
        if (!file.seek(start)) return snap;
        tail = file.read(size - start);
        if (start == 0) break;
        if (tail.count('\n') >= 2) {
            const int firstNl = tail.indexOf('\n');
            tail.remove(0, firstNl + 1);
            break;
        }
        if (window >= kMaxWindow) {
            // ANTS-1169: a single tool_result that exceeds the 4 MiB
            // window (e.g. legitimate 5 MiB inline file body) used to
            // bail and return an empty snapshot — the status bar
            // appeared frozen because no event ever survived the
            // tail-trim. Instead, fall back to the LAST complete
            // newline-delimited record in the buffer so state still
            // moves forward on giant turns. This sacrifices earlier
            // records inside the window but keeps the live-update
            // contract intact.
            QByteArray work = tail;
            // Drop a trailing newline if present so lastIndexOf below
            // finds the separator BEFORE the last record, not the
            // record's own terminator.
            if (work.endsWith('\n')) work.chop(1);
            const int lastNl = work.lastIndexOf('\n');
            tail = (lastNl >= 0) ? work.mid(lastNl + 1) : work;
            break;
        }
        window = std::min(window * 2, kMaxWindow);
    }

    QList<QJsonObject> events;
    for (const QByteArray &raw : tail.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) events.append(doc.object());
    }

    if (events.isEmpty()) return snap;
    snap.hasEvents = true;

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
            snap.contextPercent = std::min(100, inputTokens * 100 / 200000);
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
    if (!stateEvent.isEmpty()) {
        const QString type = stateEvent.value("type").toString();

        if (type == "assistant") {
            QJsonObject msg = stateEvent.value("message").toObject();
            QString stopReason = msg.value("stop_reason").toString();
            QJsonArray content = msg.value("content").toArray();

            // Detect tool_use in content blocks (more reliable than stop_reason alone)
            QString toolName;
            QJsonObject toolUseBlock;
            bool hasToolUse = false;
            for (const QJsonValue &c : content) {
                QJsonObject block = c.toObject();
                if (block.value("type").toString() == "tool_use") {
                    hasToolUse = true;
                    toolName = block.value("name").toString();
                    toolUseBlock = block;
                    break;
                }
            }

            if (hasToolUse || stopReason == "tool_use") {
                snap.state = ClaudeState::ToolUse;
                snap.tool = toolName;
                snap.toolUseBlock = toolUseBlock;
                snap.detail = toolName.isEmpty() ? QStringLiteral("tool use") : toolName;
            } else if (stopReason.isEmpty() || stopReason == "null") {
                // Still streaming — stop_reason not yet finalized
                snap.state = ClaudeState::Thinking;
                snap.detail = "thinking";
            } else {
                // end_turn, max_tokens, stop_sequence, refusal → waiting for user
                snap.state = ClaudeState::Idle;
                snap.detail = "idle";
            }
            snap.stateDetermined = true;
        } else if (type == "user" || type == "human") {
            // `user` wraps both real user messages and tool_result entries.
            // Either way Claude is processing — state is Thinking.
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
            snap.state = ClaudeState::Thinking;
            snap.detail = isToolResult ? QStringLiteral("processing result")
                                       : QStringLiteral("thinking");
            snap.stateDetermined = true;
        } else if (type == "attachment") {
            snap.state = ClaudeState::Thinking;
            snap.detail = "thinking";
            snap.stateDetermined = true;
        }
        // Any other state-determining type we don't recognize: leave
        // snap.stateDetermined == false so the caller retains its prior state.
    }

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
    // reason.
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
        snap.state = ClaudeState::Compacting;
        snap.detail = QStringLiteral("compacting");
        snap.stateDetermined = true;
    }
    snap.auditing = inAudit;

    // Plan mode: most recent permission-mode event in the tail decides.
    // Claude Code records `{"type":"permission-mode","permissionMode":"plan",
    // "sessionId":"…"}` when the user toggles plan mode; switching out
    // writes another with permissionMode == "default" / "acceptEdits" /
    // "bypassPermissions". Field name is permissionMode (verified against
    // live JSONL on disk as of Claude Code v2.1.87); the pre-0.7.12 code
    // read "mode" which never matched the real schema — see
    // tests/features/claude_plan_mode_detection/spec.md.
    //
    // Important: the tail window we parse is ~32 KB, so a toggle that
    // happened many turns ago can scroll off. We must NOT silently reset
    // plan mode to false in that case — the user's last explicit toggle
    // still stands until they toggle again. Caller passes the latched
    // value; we override only when we actually observe a permission-mode
    // event in the window.
    for (int i = events.size() - 1; i >= 0; --i) {
        if (events[i].value("type").toString() != QLatin1String("permission-mode"))
            continue;
        const QString mode = events[i].value("permissionMode").toString();
        snap.planMode = (mode == QLatin1String("plan"));
        break;
    }

    return snap;
}

void ClaudeIntegration::parseTranscriptForState(const QString &path) {
    // Re-add the watch — QFileSystemWatcher can drop it after atomic saves
    if (!m_transcriptWatcher.files().contains(path))
        m_transcriptWatcher.addPath(path);

    const ClaudeTranscriptSnapshot snap = parseTranscriptTail(path, m_planMode);
    if (!snap.hasEvents) return;

    if (snap.contextPercent >= 0) {
        m_contextPercent = snap.contextPercent;
        emit contextUpdated(m_contextPercent);
    }

    if (!snap.toolUseBlock.isEmpty()) {
        updateChangedFiles(snap.toolUseBlock);
    }

    if (snap.planMode != m_planMode) {
        m_planMode = snap.planMode;
        // Mirror into the per-shellPid cache so a future tab switch
        // away-and-back restores the latched state. See setShellPid
        // for the read side. 0.7.54.
        if (m_shellPid > 0) m_planModeByPid[m_shellPid] = m_planMode;
        emit planModeChanged(m_planMode);
    }
    if (snap.auditing != m_auditing) {
        m_auditing = snap.auditing;
        emit auditingChanged(m_auditing);
    }

    // Only apply state if the tail actually determined one — otherwise
    // retain m_state (an unrecognized trailing event must not clobber
    // a live state). Matches pre-refactor behavior of the newState=m_state
    // initialization.
    if (snap.stateDetermined && snap.state != m_state) {
        m_state = snap.state;
        m_currentTool = snap.tool;
        emit stateChanged(m_state, snap.detail);
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
    // ANTS-1132 — UserAccessOption applies the 0700 perms at the
    // socket layer BEFORE bind+listen, closing the TOCTOU window
    // between listen() and the post-listen setOwnerOnlyPerms call.
    // Matches the remote-control trust model.
    m_hookServer->setSocketOptions(QLocalServer::UserAccessOption);
    QString socketPath = QDir::tempPath() + "/ants-claude-hooks-" +
                         QString::number(QCoreApplication::applicationPid());
    // ANTS-1132 — gate removeServer() behind the lstat-checked
    // S_ISSOCK + UID match guard. Without this, a hostile same-UID
    // process could pre-create a symlink at socketPath pointing at
    // e.g. ~/.ssh/known_hosts and removeServer would unlink the
    // target. Same defence-in-depth shape as remotecontrol.
    if (!safeToUnlinkLocalSocket(socketPath)) {
        delete m_hookServer;
        m_hookServer = nullptr;
        return false;
    }
    QLocalServer::removeServer(socketPath);

    if (!m_hookServer->listen(socketPath)) {
        delete m_hookServer;
        m_hookServer = nullptr;
        return false;
    }
    // Restrict socket permissions to owner only (belt-and-braces
    // alongside UserAccessOption above).
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
        // ANTS-1151 — extend the SO_PEERCRED + idle-timeout pattern
        // from RemoteControl::onNewConnection to the Claude hook
        // socket. UserAccessOption + safeToUnlinkLocalSocket
        // already cover the file-side guarantees, but the peer
        // side needs explicit getsockopt(SO_PEERCRED) — a
        // same-UID-but-different-process attacker (e.g. a
        // malicious browser plugin) could otherwise inject hook
        // events shaped like processHookEvent consumes.
        const qintptr fd = socket->socketDescriptor();
        if (fd >= 0) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            if (::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                             SO_PEERCRED, &cred, &len) != 0 ||
                cred.uid != ::getuid()) {
                socket->disconnectFromServer();
                socket->deleteLater();
                continue;
            }
        }
        // ANTS-1151 — slow-loris defence. A peer that connects
        // and never sends bytes (or never closes) used to hold a
        // QLocalSocket forever. 5 s idle timeout matches
        // RemoteControl.
        //
        // Indie-review-2026-05-14 lane-3 H1 — LOAD-BEARING: do NOT
        // restart this timer inside readyRead to "extend on activity."
        // The 5 s cap is the only wall-clock bound on a single
        // hook-server RPC; the JSON parse happens once on
        // disconnect against up to 10 MiB of buffered data. A peer
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

    // Indie-review 2026-05-13: cold-start tightening. During the 1-3s
    // window between setShellPid()'s synchronous m_transcriptPath
    // clear and the next pollClaudeProcess tick, isFocusedTabSession
    // returns true for ANY incoming event. PreToolUse/PostToolUse/Stop
    // from a sibling tab's still-running Claude would mutate state
    // and emit stateChanged on the singleton. Only SessionStart needs
    // the cold-start fallthrough (it bootstraps m_activeSessionId);
    // every other state-mutating hook should drop silently until poll
    // resolves the transcript path.
    const bool coldStart = m_transcriptPath.isEmpty();
    const bool isStateMutatingHook = hookName != "SessionStart" &&
                                     hookName != "PermissionRequest";
    if (coldStart && isStateMutatingHook) {
        if (DebugLog::enabled(DebugLog::Claude)) {
            ANTS_LOG(DebugLog::Claude,
                     "hook-drop (cold-start): session=%s hook=%s",
                     incomingSessionId.toUtf8().constData(),
                     hookName.toUtf8().constData());
        }
        return;  // Drop without leaking session-id into routing field.
    }

    // The event passed the cold-start gate; commit to last-seen.
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
        updateChangedFiles(event);
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

    m_mcpServer = new QLocalServer(this);
    // ANTS-1132 — same trust-model pre-checks as the hook server.
    m_mcpServer->setSocketOptions(QLocalServer::UserAccessOption);
    if (!safeToUnlinkLocalSocket(socketPath)) {
        delete m_mcpServer;
        m_mcpServer = nullptr;
        return false;
    }
    QLocalServer::removeServer(socketPath);

    if (!m_mcpServer->listen(socketPath)) {
        delete m_mcpServer;
        m_mcpServer = nullptr;
        return false;
    }
    // Restrict socket permissions to owner only (belt-and-braces
    // alongside UserAccessOption above).
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

// ANTS-1253: single tool-provider registrar. Replaces the 12 per-tool
// setXProvider bodies that ANTS-1244..1251 each added. Each handler
// receives the JSON-RPC `arguments` object and returns the tool's
// response as a JSON string; the dispatcher in onMcpConnection wraps
// it into a text content block.
void ClaudeIntegration::registerToolProvider(
    const QString &name, ToolHandler handler) {
    m_toolProviders[name] = std::move(handler);
}

void ClaudeIntegration::onMcpConnection() {
    while (m_mcpServer->hasPendingConnections()) {
        QLocalSocket *socket = m_mcpServer->nextPendingConnection();
        // ANTS-1151 — same SO_PEERCRED + idle-timeout pattern as
        // onHookConnection. MCP socket carries higher-leverage
        // verbs (filesystem reads, git status, environment),
        // peer-cred check is more important here.
        const qintptr fd = socket->socketDescriptor();
        if (fd >= 0) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            if (::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                             SO_PEERCRED, &cred, &len) != 0 ||
                cred.uid != ::getuid()) {
                socket->disconnectFromServer();
                socket->deleteLater();
                continue;
            }
        }
        QTimer *idleTimer = new QTimer(socket);
        idleTimer->setSingleShot(true);
        idleTimer->setInterval(5000);
        connect(idleTimer, &QTimer::timeout, socket,
                [socket]() { socket->abort(); });
        idleTimer->start();
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
                // ANTS-1284 — reset per-tool dispatch counters on
                // session-start handshake. See docs/specs/ANTS-1284.md.
                m_tokenUsage.reset();
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

                // MCP spec requires every tool to declare an inputSchema
                // (even zero-arg tools — empty object schema). Claude
                // Code's Zod validator rejects the whole tools/list
                // response if any entry omits it, silently registering
                // zero tools.
                QJsonObject emptySchema;
                emptySchema["type"] = "object";

                QJsonObject cwdTool;
                cwdTool["name"] = "get_cwd";
                cwdTool["description"] = "Get the terminal's current working directory";
                cwdTool["inputSchema"] = emptySchema;
                tools.append(cwdTool);

                QJsonObject sessionTool;
                sessionTool["name"] = "get_session_info";
                sessionTool["description"] = "Get terminal session metadata";
                sessionTool["inputSchema"] = emptySchema;
                tools.append(sessionTool);

                QJsonObject lastCmdTool;
                lastCmdTool["name"] = "get_last_command";
                lastCmdTool["description"] = "Get the last command's exit code and output (via shell integration)";
                lastCmdTool["inputSchema"] = emptySchema;
                tools.append(lastCmdTool);

                QJsonObject gitTool;
                gitTool["name"] = "get_git_status";
                gitTool["description"] = "Get git branch, status, and recent commits for the terminal's CWD";
                gitTool["inputSchema"] = emptySchema;
                tools.append(gitTool);

                QJsonObject envTool;
                envTool["name"] = "get_environment";
                envTool["description"] = "Get shell environment info (PATH, virtualenv, key env vars)";
                envTool["inputSchema"] = emptySchema;
                tools.append(envTool);

                // ANTS-1244 — surface 3 existing remote-control verbs
                // as MCP tools so a Claude session in an Ants tab can
                // query terminal state via tool-call rather than
                // `Bash`/`Read`. See docs/specs/ANTS-1244.md.
                //
                // ANTS-1247-INV-8: `status` filter declared in inputSchema
                // with enum [all|active|shipped] + description citing
                // token counts so Claude prefers `active` for planning
                // queries (~10× smaller payload).
                QJsonObject roadmapTool;
                roadmapTool["name"] = "roadmap_query";
                // ANTS-1287 — `section` arg added. Description cites
                // the partial-query saving so Claude prefers section
                // slices when only one block is needed.
                roadmapTool["description"] = QStringLiteral(
                    "Query the active tab's ROADMAP.md as structured "
                    "bullets. Each bullet: {id, status, headline, kind, "
                    "lanes}. Optional `status` filter — \"active\" "
                    "(📋+🚧, ~1.7 K tokens — recommended for planning "
                    "queries) / \"shipped\" (✅ only) / \"all\" (default, "
                    "~12 K tokens). Optional `section` slug — returns "
                    "only bullets within that ## or ### heading "
                    "(e.g. \"performance\", \"080\"); response carries "
                    "`section` echo. Envelope: {ok, bullets, path, "
                    "count, filter, section?} on success or "
                    "{ok:false, error, code} on error.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject statusProp;
                    statusProp["type"] = "string";
                    QJsonArray statusEnum;
                    statusEnum.append("all");
                    statusEnum.append("active");
                    statusEnum.append("shipped");
                    statusProp["enum"] = statusEnum;
                    statusProp["description"] = QStringLiteral(
                        "Filter by lifecycle. \"active\" = planned + "
                        "in-progress (~7× smaller payload).");
                    props["status"] = statusProp;
                    // ANTS-1287 — `section` slug (optional). Unknown
                    // slug → ok:false with code=bad_section.
                    QJsonObject sectionProp;
                    sectionProp["type"] = "string";
                    sectionProp["description"] = QStringLiteral(
                        "Slug of a ## or ### heading (e.g. "
                        "\"performance-2\"). Returns only bullets "
                        "within that section; saves a full reparse "
                        "for partial queries. Unknown slug → "
                        "code=bad_section.");
                    props["section"] = sectionProp;
                    schema["properties"] = props;
                    roadmapTool["inputSchema"] = schema;
                }
                tools.append(roadmapTool);

                QJsonObject tabListTool;
                tabListTool["name"] = "tab_list";
                tabListTool["description"] = QStringLiteral(
                    "List all open terminal tabs in this Ants instance. "
                    "Each tab: {index, title, cwd, shell_pid, "
                    "claude_running, color}. Envelope: {ok:true, tabs:[…]}.");
                tabListTool["inputSchema"] = emptySchema;
                tools.append(tabListTool);

                QJsonObject getTextTool;
                getTextTool["name"] = "get_text";
                getTextTool["description"] = QStringLiteral(
                    "Read trailing scrollback lines from a tab. Optional "
                    "`tab` (default = active tab) and `lines` (default 100, "
                    "capped at 10000). Returns {ok, text, lines, bytes} or "
                    "{ok:false, error} when the tab index is out of range.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject tabProp;   tabProp["type"]   = "integer";
                    QJsonObject linesProp; linesProp["type"] = "integer";
                                           linesProp["default"] = 100;
                    props["tab"]   = tabProp;
                    props["lines"] = linesProp;
                    schema["properties"] = props;
                    getTextTool["inputSchema"] = schema;
                }
                tools.append(getTextTool);

                // ANTS-1248: workspace_search — ripgrep wrapper. The
                // schema declares all 7 spec args and marks `pattern`
                // as required. The description names the alternative
                // bash idiom + token-saving headline so Claude prefers
                // this over Bash/Read for code searches.
                QJsonObject wsTool;
                wsTool["name"] = "workspace_search";
                wsTool["description"] = QStringLiteral(
                    "Search the project for code matching a literal "
                    "string or regex. Returns {ok, matches:[{file, "
                    "line, text}], truncated, elapsed_ms}. Prefer this "
                    "over `Bash grep -r ...` — typically saves 250-4500 "
                    "tokens per query and avoids round-trips for "
                    "no-match cases. Args: pattern (required), regex "
                    "(false), lane (subdir under project root), glob, "
                    "max_results (default 50, cap 500), context, case "
                    "(smart/sensitive/insensitive).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject patternProp;  patternProp["type"]  = "string";
                                              patternProp["description"] =
                        QStringLiteral("Literal string (default) or "
                                        "regex if `regex=true`.");
                    QJsonObject regexProp;    regexProp["type"]    = "boolean";
                                              regexProp["default"] = false;
                    QJsonObject laneProp;     laneProp["type"]     = "string";
                                              laneProp["description"] =
                        QStringLiteral("Subdir under repo root (e.g. \"src\"). "
                                        "Empty = whole repo.");
                    QJsonObject globProp;     globProp["type"]     = "string";
                                              globProp["description"] =
                        QStringLiteral("Ripgrep --glob filter (e.g. \"*.cpp\").");
                    QJsonObject maxProp;      maxProp["type"]      = "integer";
                                              maxProp["default"]   = 50;
                                              maxProp["maximum"]   = 500;
                    QJsonObject ctxProp;      ctxProp["type"]      = "integer";
                                              ctxProp["default"]   = 0;
                    QJsonObject caseProp;     caseProp["type"]     = "string";
                    QJsonArray caseEnum;
                    caseEnum.append("smart");
                    caseEnum.append("sensitive");
                    caseEnum.append("insensitive");
                    caseProp["enum"]    = caseEnum;
                    caseProp["default"] = "smart";
                    props["pattern"]     = patternProp;
                    props["regex"]       = regexProp;
                    props["lane"]        = laneProp;
                    props["glob"]        = globProp;
                    props["max_results"] = maxProp;
                    props["context"]     = ctxProp;
                    props["case"]        = caseProp;
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("pattern");
                    schema["required"] = required;
                    wsTool["inputSchema"] = schema;
                }
                tools.append(wsTool);

                // ANTS-1249: file_outline — regex-scanner outline for
                // a single file. ~13-39× compression on a typical
                // source file vs full Read. Schema declares `path`
                // required + 3 optional knobs.
                QJsonObject foTool;
                foTool["name"] = "file_outline";
                foTool["description"] = QStringLiteral(
                    "Return a structured outline of a file — header "
                    "comment + per-symbol {line, kind, name, "
                    "signature}. Prefer this over a full Read when "
                    "you only need to know what's IN a file (where "
                    "to grep, which class lives in what file). "
                    "Languages: cpp / py / md (auto-picked by "
                    "extension); other types still report "
                    "total_lines/total_bytes for orientation. "
                    "Typically 13-39× smaller than a full Read.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp;     pathProp["type"]     = "string";
                                              pathProp["description"] =
                        QStringLiteral("Repo-relative or absolute path. "
                                       "Must resolve under project root.");
                    QJsonObject modeProp;     modeProp["type"]     = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("auto");
                    modeEnum.append("cpp");
                    modeEnum.append("py");
                    modeEnum.append("md");
                    modeEnum.append("json");
                    modeProp["enum"]    = modeEnum;
                    modeProp["default"] = "auto";
                    QJsonObject hdrProp;      hdrProp["type"]      = "boolean";
                                              hdrProp["default"]   = true;
                    QJsonObject maxSymProp;   maxSymProp["type"]   = "integer";
                                              maxSymProp["default"] = 200;
                                              maxSymProp["maximum"] = 1000;
                    props["path"]                 = pathProp;
                    props["mode"]                 = modeProp;
                    props["include_doc_comment"]  = hdrProp;
                    props["max_symbols"]          = maxSymProp;
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("path");
                    schema["required"] = required;
                    foTool["inputSchema"] = schema;
                }
                tools.append(foTool);

                // ANTS-1250: git_state — single tool, dispatches on
                // `op` (status / log / diff). Collapsed from three
                // separate tools to save ~240 permanent schema tokens
                // per session-start (cold-eyes pass 2).
                QJsonObject gsTool;
                gsTool["name"] = "git_state";
                gsTool["description"] = QStringLiteral(
                    "Query git repo state (status / log / diff) as "
                    "structured JSON. Replaces multiple Bash "
                    "invocations of `git status` / `git log` / "
                    "`git diff`. Required: op (\"status\" / \"log\" / "
                    "\"diff\"). Op-specific: n (log only, default 10, "
                    "cap 100), path (log/diff filter), range (diff "
                    "only, e.g. HEAD~5..HEAD), body (log only, "
                    "include commit body). Saves ~14-300 tokens per "
                    "call vs Bash.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject opProp;     opProp["type"]    = "string";
                    QJsonArray opEnum;
                    opEnum.append("status");
                    opEnum.append("log");
                    opEnum.append("diff");
                    opProp["enum"]    = opEnum;
                    QJsonObject nProp;      nProp["type"]     = "integer";
                                            nProp["default"]  = 10;
                                            nProp["minimum"]  = 1;
                                            nProp["maximum"]  = 100;
                                            nProp["description"] =
                        QStringLiteral("log only");
                    QJsonObject pathProp;   pathProp["type"]  = "string";
                                            pathProp["description"] =
                        QStringLiteral("log/diff: filter to repo-relative path");
                    QJsonObject rangeProp;  rangeProp["type"] = "string";
                                            rangeProp["description"] =
                        QStringLiteral("diff: e.g. HEAD~5..HEAD");
                    QJsonObject bodyProp;   bodyProp["type"]  = "boolean";
                                            bodyProp["default"] = false;
                                            bodyProp["description"] =
                        QStringLiteral("log: include commit body");
                    props["op"]    = opProp;
                    props["n"]     = nProp;
                    props["path"]  = pathProp;
                    props["range"] = rangeProp;
                    props["body"]  = bodyProp;
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("op");
                    schema["required"] = required;
                    gsTool["inputSchema"] = schema;
                }
                tools.append(gsTool);

                // ANTS-1251: subsystem — single tool, dispatches on
                // `op` (map / files / recent_changes). Pre-parses the
                // CLAUDE.md Module map and serves per-lane chunks so
                // /indie-review reviewers don't each re-read the file.
                QJsonObject ssTool;
                ssTool["name"] = "subsystem";
                ssTool["description"] = QStringLiteral(
                    "Query the project's subsystem (lane) map parsed "
                    "from CLAUDE.md. Three ops: map (all lanes), "
                    "files (per-lane file list), recent_changes "
                    "(per-lane git log). Required: op. lane required "
                    "for files / recent_changes. n: recent_changes "
                    "only (default 10, cap 100). Saves ~24 K tokens "
                    "per /indie-review run.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject opProp;     opProp["type"]    = "string";
                    QJsonArray opEnum;
                    opEnum.append("map");
                    opEnum.append("files");
                    opEnum.append("recent_changes");
                    opProp["enum"]    = opEnum;
                    QJsonObject laneProp;   laneProp["type"]  = "string";
                                            laneProp["description"] =
                        QStringLiteral("required for files / recent_changes");
                    QJsonObject nProp;      nProp["type"]     = "integer";
                                            nProp["default"]  = 10;
                                            nProp["minimum"]  = 1;
                                            nProp["maximum"]  = 100;
                                            nProp["description"] =
                        QStringLiteral("recent_changes only");
                    props["op"]   = opProp;
                    props["lane"] = laneProp;
                    props["n"]    = nProp;
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("op");
                    schema["required"] = required;
                    ssTool["inputSchema"] = schema;
                }
                tools.append(ssTool);

                // ANTS-1254: last_audit_summary — opens the latest
                // .audit_cache/audit-*.sarif and returns counts +
                // top_findings. ~5-15 K tokens → ~250 saved per fire.
                QJsonObject lasTool;
                lasTool["name"] = "last_audit_summary";
                lasTool["description"] = QStringLiteral(
                    "Read the latest audit-*.sarif under "
                    "{cwd}/.audit_cache and return a compact summary: "
                    "counts (error/warning/note/suppressed) plus "
                    "top_findings (sorted by level desc, confidence "
                    "desc, file asc, line asc). Saves ~5-15 K tokens "
                    "vs reading the HTML report. Returns "
                    "{ok:false, code:\"not_audited\"} if no SARIF.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject topNProp;
                    topNProp["type"]    = "integer";
                    topNProp["default"] = 5;
                    topNProp["minimum"] = 0;
                    topNProp["maximum"] = 50;
                    topNProp["description"] = QStringLiteral(
                        "Cap on top_findings[]. Server-clamp [0, 50].");
                    QJsonObject floorProp;
                    floorProp["type"]    = "string";
                    floorProp["default"] = "warning";
                    QJsonArray floorEnum;
                    floorEnum.append("error");
                    floorEnum.append("warning");
                    floorEnum.append("note");
                    floorProp["enum"]    = floorEnum;
                    floorProp["description"] = QStringLiteral(
                        "SARIF level floor for top_findings[]. "
                        "Findings below this are still counted.");
                    props["top_n"]          = topNProp;
                    props["severity_floor"] = floorProp;
                    schema["properties"]    = props;
                    lasTool["inputSchema"] = schema;
                }
                tools.append(lasTool);

                // ANTS-1112 — five `indie_review_*` tools that lift the
                // mechanical halves of /indie-review out of orchestrator
                // context. Engine: src/indiereviewengine.{h,cpp}.
                {
                    QJsonObject t;
                    t["name"] = "indie_review_partition";
                    t["description"] = QStringLiteral(
                        "Return the subsystem (lane) partition for "
                        "indie-review. Reads CLAUDE.md `## Module map "
                        "(src/)` (or the `.indie-review/partition.json` "
                        "override if present) and computes per-lane "
                        "source-file lists. Saves N file-walk passes "
                        "the orchestrator would otherwise do.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["properties"] = QJsonObject{};
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "indie_review_brief";
                    t["description"] = QStringLiteral(
                        "Return a brief manifest for one lane: "
                        "prompt template + source-path list + "
                        "contract-doc list (ANTS-1281). Response "
                        "fields: brief, source_paths[], "
                        "contract_docs[], external_specs[], "
                        "dimension_weighting{}. Source bodies are "
                        "NOT inlined; the subagent reads them via "
                        "its Read tool. Saves ~10-30 K orchestrator "
                        "tokens per lane vs the v1 shape that "
                        "inlined bodies. Pure file IO (no LLM). "
                        "Required: lane (string).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject laneProp;
                    laneProp["type"] = "string";
                    laneProp["description"] = QStringLiteral(
                        "Lane name as returned by indie_review_partition.");
                    QJsonObject props;
                    props["lane"] = laneProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("lane");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "indie_review_corroborate";
                    t["description"] = QStringLiteral(
                        "Cross-lane corroboration filter. Input: "
                        "EITHER `reports` (inline map of "
                        "{lane: report_text}, v1) OR `reports_dir` "
                        "(project-relative directory of *.md files, "
                        "v2 — saves parent context by reading from "
                        "disk server-side; ANTS-1282). Returns "
                        "findings cited by >= min_lanes distinct "
                        "lanes at the same (file, line). Pure regex "
                        "pass; no LLM. Provide exactly one of "
                        "`reports` or `reports_dir`. Optional: "
                        "min_lanes (default 2).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject reportsProp;
                    reportsProp["type"] = "object";
                    reportsProp["description"] = QStringLiteral(
                        "Map of {lane_name: report_markdown}. "
                        "Mutually exclusive with reports_dir.");
                    QJsonObject reportsDirProp;
                    reportsDirProp["type"] = "string";
                    reportsDirProp["description"] = QStringLiteral(
                        "Project-relative path to a directory of "
                        "*.md files. Lane name = filename stem. "
                        "Top level only; sub-directories not "
                        "recursed. Files >64 KiB truncated. "
                        "Mutually exclusive with reports.");
                    QJsonObject minLanesProp;
                    minLanesProp["type"]    = "integer";
                    minLanesProp["default"] = 2;
                    minLanesProp["minimum"] = 1;
                    minLanesProp["description"] = QStringLiteral(
                        "Minimum distinct lanes citing a (file, line) "
                        "for it to count as corroborated.");
                    QJsonObject props;
                    props["reports"]     = reportsProp;
                    props["reports_dir"] = reportsDirProp;
                    props["min_lanes"]   = minLanesProp;
                    schema["properties"] = props;
                    // INV-1 XOR is enforced at the handler layer
                    // (cmdIndieReviewCorroborate), not the schema —
                    // JSON Schema's oneOf is verbose and Claude
                    // Code's schema validator handles oneOf poorly.
                    // The handler returns bad_args if both or
                    // neither is provided.
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "indie_review_synthesis_prompt";
                    t["description"] = QStringLiteral(
                        "Render the synthesis-prompt template for the "
                        "optional cross-cutting LLM call. Combines "
                        "per-lane reports + threat-model extras "
                        "(CLAUDE.md + SECURITY.md + .semgrep.yml). "
                        "Caller dispatches the prompt. Required: "
                        "reports (object). Optional: "
                        "include_threat_model_extras (default true).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject reportsProp;
                    reportsProp["type"] = "object";
                    QJsonObject incProp;
                    incProp["type"]    = "boolean";
                    incProp["default"] = true;
                    QJsonObject props;
                    props["reports"]                    = reportsProp;
                    props["include_threat_model_extras"] = incProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("reports");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "indie_review_fold_in";
                    t["description"] = QStringLiteral(
                        "Render an `### 🔍 Indie-review fold-in (DATE)` "
                        "ROADMAP block from a list of corroborated "
                        "findings. Allocates IDs from .roadmap-counter "
                        "(via RoadmapFoldIn::allocateIds) and, if a "
                        "release-block heading is found via "
                        "findActiveReleaseHeading, atomically inserts "
                        "the block into ROADMAP.md. Required: "
                        "actionable (array).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject actProp;
                    actProp["type"] = "array";
                    actProp["description"] = QStringLiteral(
                        "Array of {file, line, citing_lanes[]} "
                        "objects describing the corroborated set.");
                    QJsonObject dateProp;
                    dateProp["type"] = "string";
                    dateProp["description"] = QStringLiteral(
                        "ISO date for the heading + Source. Defaults "
                        "to today.");
                    QJsonObject hdrProp;
                    hdrProp["type"] = "string";
                    hdrProp["description"] = QStringLiteral(
                        "Optional explicit `## ` heading to insert "
                        "after; defaults to "
                        "RoadmapFoldIn::findActiveReleaseHeading.");
                    QJsonObject props;
                    props["actionable"]             = actProp;
                    props["date_iso"]               = dateProp;
                    props["release_block_heading"]  = hdrProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("actionable");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1113 — debt_sweep_* (4 tools).
                {
                    QJsonObject t;
                    t["name"] = "debt_sweep_scan";
                    t["description"] = QStringLiteral(
                        "Run the four-category mechanical debt-sweep scan "
                        "(code_drift, test_coverage, doc_drift, "
                        "packaging_drift) and return findings as JSON. "
                        "Replaces the file-reading subagent in the "
                        "/debt-sweep skill for Ants-managed projects. "
                        "Optional: since (git ref, default = most-recent "
                        "tag or HEAD~10), categories (subset of the four).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject sinceProp;
                    sinceProp["type"] = "string";
                    sinceProp["description"] = QStringLiteral(
                        "Git ref to scope diffs against. Empty = auto.");
                    QJsonObject catProp;
                    catProp["type"] = "array";
                    catProp["description"] = QStringLiteral(
                        "Subset of {code_drift, test_coverage, "
                        "doc_drift, packaging_drift}. Omit for all.");
                    QJsonObject props;
                    props["since"]      = sinceProp;
                    props["categories"] = catProp;
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "debt_sweep_apply_fix";
                    t["description"] = QStringLiteral(
                        "Apply ONE mechanical fix in-place. Caller passes "
                        "the {detector_id, file, line} triple from a "
                        "prior debt_sweep_scan. Engine re-validates the "
                        "marker is still on the line before mutating. "
                        "Returns {ok, applied, error_code?, error?}; "
                        "ok=true with applied=false signals a recognised "
                        "no-op (file_changed / not_fixable).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject didProp; didProp["type"] = "string";
                    QJsonObject fProp;   fProp["type"]   = "string";
                    QJsonObject lProp;   lProp["type"]   = "integer";
                    QJsonObject aProp;   aProp["type"]   = "boolean";
                    aProp["description"] = QStringLiteral(
                        "Caller asserts the finding was auto_fixable in "
                        "the prior scan. Defaults to true.");
                    QJsonObject props;
                    props["detector_id"]  = didProp;
                    props["file"]         = fProp;
                    props["line"]         = lProp;
                    props["auto_fixable"] = aProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("detector_id");
                    req.append("file");
                    req.append("line");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "debt_sweep_defer";
                    t["description"] = QStringLiteral(
                        "Render an `### 🧹 Debt-sweep fold-in (DATE)` "
                        "ROADMAP block from a list of deferred findings. "
                        "Allocates IDs from .roadmap-counter and, if a "
                        "release-block heading is found, atomically "
                        "inserts the block into ROADMAP.md. Required: "
                        "deferred (array).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject dProp; dProp["type"] = "array";
                    dProp["description"] = QStringLiteral(
                        "Array of Finding-shaped objects ({category, "
                        "detector_id, file, line, message}).");
                    QJsonObject dateProp; dateProp["type"] = "string";
                    dateProp["description"] = QStringLiteral(
                        "ISO date YYYY-MM-DD. Defaults to today.");
                    QJsonObject hdrProp; hdrProp["type"] = "string";
                    hdrProp["description"] = QStringLiteral(
                        "Optional explicit `## ` heading. Defaults to "
                        "RoadmapFoldIn::findActiveReleaseHeading.");
                    QJsonObject props;
                    props["deferred"]              = dProp;
                    props["date_iso"]              = dateProp;
                    props["release_block_heading"] = hdrProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("deferred");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "debt_sweep_triage_prompt";
                    t["description"] = QStringLiteral(
                        "Render an LLM triage prompt for the LLM-shaped "
                        "(non-mechanical) subset of findings. Caller "
                        "filters the scan output and passes only the "
                        "judgment-required entries. Pure string "
                        "templating — no LLM call inside Ants.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject fProp; fProp["type"] = "array";
                    fProp["description"] = QStringLiteral(
                        "Array of Finding-shaped objects.");
                    QJsonObject props;
                    props["findings"] = fProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("findings");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1289 — verify_changes
                {
                    QJsonObject t;
                    t["name"] = "verify_changes";
                    t["description"] = QStringLiteral(
                        "Run the project's build / test / lint gates "
                        "and return pass/fail with log tails. Replaces "
                        "the verification-before-completion skill's "
                        "mechanical loop. Reads .ants/verify.json (or "
                        "auto-detects from CMakePresets/package.json/"
                        "Cargo.toml/pyproject.toml). Pure shell-out. "
                        "No required args.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject gatesProp;
                    gatesProp["type"] = "array";
                    gatesProp["description"] = QStringLiteral(
                        "Subset of [\"build\",\"tests\",\"lint\"] to "
                        "run. Empty / omitted = all configured gates.");
                    QJsonObject linesProp;
                    linesProp["type"] = "integer";
                    linesProp["description"] = QStringLiteral(
                        "Per-gate log-tail line cap. Server-clamped "
                        "[10, 500]; default 50.");
                    QJsonObject timeoutProp;
                    timeoutProp["type"] = "integer";
                    timeoutProp["description"] = QStringLiteral(
                        "Total wall-clock budget in seconds, split "
                        "evenly across configured gates. Server-"
                        "clamped [10, 1800]; default 1200.");
                    QJsonObject props;
                    props["gates"]         = gatesProp;
                    props["max_log_lines"] = linesProp;
                    props["timeout_sec"]   = timeoutProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1290 — plan_template
                {
                    QJsonObject t;
                    t["name"] = "plan_template";
                    t["description"] = QStringLiteral(
                        "Emit a project-conventional implementation-"
                        "plan skeleton with placeholders for the body. "
                        "Replaces the writing-plans skill's template "
                        "emission. Returns markdown + suggested save "
                        "path + project conventions (commit format, "
                        "test scaffolding, build commands). Optionally "
                        "writes to docs/plans/ when save:true. "
                        "Required: feature_name. Optional: goal, "
                        "architecture, tech_stack, task_count_hint, "
                        "includes_tests, ants_id, save.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject featProp;
                    featProp["type"] = "string";
                    featProp["description"] = QStringLiteral(
                        "Kebab-case feature name (^[a-z0-9_-]+$, max "
                        "64 chars). Becomes the h1 + test-dir name.");
                    QJsonObject goalProp;
                    goalProp["type"] = "string";
                    goalProp["description"] = QStringLiteral(
                        "One-sentence \"what this builds\". Empty = "
                        "placeholder in the skeleton.");
                    QJsonObject archProp;
                    archProp["type"] = "string";
                    archProp["description"] = QStringLiteral(
                        "2-3 sentences on the approach. Empty = "
                        "placeholder.");
                    QJsonObject stackProp;
                    stackProp["type"] = "string";
                    stackProp["description"] = QStringLiteral(
                        "Key technologies/libraries. Empty = "
                        "placeholder.");
                    QJsonObject countProp;
                    countProp["type"] = "integer";
                    countProp["description"] = QStringLiteral(
                        "Number of task blocks to emit. Server-"
                        "clamped [1, 12]; default 4.");
                    QJsonObject testsProp;
                    testsProp["type"] = "boolean";
                    testsProp["description"] = QStringLiteral(
                        "Whether to include test-scaffolding hints in "
                        "the skeleton. Default true.");
                    QJsonObject antsProp;
                    antsProp["type"] = "string";
                    antsProp["description"] = QStringLiteral(
                        "Pre-allocated ANTS-NNNN id. Empty = engine "
                        "allocates from .roadmap-counter.");
                    QJsonObject saveProp;
                    saveProp["type"] = "boolean";
                    saveProp["description"] = QStringLiteral(
                        "If true, also writes the skeleton to "
                        "docs/plans/<id>-<feature>.md atomically. "
                        "Refuses to overwrite. Default false.");
                    QJsonObject props;
                    props["feature_name"]    = featProp;
                    props["goal"]            = goalProp;
                    props["architecture"]    = archProp;
                    props["tech_stack"]      = stackProp;
                    props["task_count_hint"] = countProp;
                    props["includes_tests"]  = testsProp;
                    props["ants_id"]         = antsProp;
                    props["save"]            = saveProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("feature_name"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1284 — token_usage
                {
                    QJsonObject t;
                    t["name"] = "token_usage";
                    t["description"] = QStringLiteral(
                        "Reports per-tool token-saving telemetry for "
                        "this MCP server's current session. Returns "
                        "{since, calls:[...], total_saved, "
                        "tools_called}; calls sorted by "
                        "est_tokens_saved descending. Pure read by "
                        "default; pass reset:true to read-and-clear "
                        "in one round-trip. No required args.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject resetProp;
                    resetProp["type"] = "boolean";
                    resetProp["description"] = QStringLiteral(
                        "If true, clears all counters AFTER building "
                        "the report. Default false.");
                    QJsonObject zeroProp;
                    zeroProp["type"] = "boolean";
                    zeroProp["description"] = QStringLiteral(
                        "If true, include tools with "
                        "est_tokens_saved == 0 in calls[]. Default "
                        "false.");
                    QJsonObject props;
                    props["reset"]        = resetProp;
                    props["include_zero"] = zeroProp;
                    schema["properties"] = props;
                    schema["required"] = QJsonArray();
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1319 — cold_eyes_* (4 tools). Mirror to indie_review /
                // debt_sweep fold-in pattern; pure delegation to
                // ColdEyesEngine + RoadmapFoldIn helpers.
                {
                    QJsonObject t;
                    t["name"] = "cold_eyes_partition";
                    t["description"] = QStringLiteral(
                        "Return the doc-lane partition for /cold-eyes. "
                        "Walks docs/ + root contracts, groups by topic "
                        "cohesion (contracts / standards / decisions / "
                        "plugins / per-active-spec). Optional override: "
                        "<projectPath>/.cold-eyes/partition.json. "
                        "Spec-lanes capped at 12 (most-recently-modified). "
                        "Optional: scope (\"default\" / \"docs_only\" / "
                        "\"contracts_only\").");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject scopeProp;
                    scopeProp["type"] = "string";
                    QJsonArray scopeEnum;
                    scopeEnum.append("default");
                    scopeEnum.append("docs_only");
                    scopeEnum.append("contracts_only");
                    scopeProp["enum"] = scopeEnum;
                    scopeProp["default"] = "default";
                    scopeProp["description"] = QStringLiteral(
                        "Partition scope. \"default\" emits the full "
                        "lane set; \"docs_only\" omits the root "
                        "contract lane; \"contracts_only\" emits ONLY "
                        "the contract lane.");
                    QJsonObject props;
                    props["scope"] = scopeProp;
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "cold_eyes_brief";
                    t["description"] = QStringLiteral(
                        "Return a brief manifest for one lane: brief "
                        "text + doc_paths + cross_reference_docs + "
                        "cited_code_paths. Doc bodies are NOT inlined "
                        "(ANTS-1319 INV-3, mirrors ANTS-1281); the "
                        "subagent reads each doc via its Read tool. "
                        "Required: lane (string).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject laneProp;
                    laneProp["type"] = "string";
                    laneProp["description"] = QStringLiteral(
                        "Lane name as returned by cold_eyes_partition.");
                    QJsonObject props;
                    props["lane"] = laneProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("lane");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "cold_eyes_cross_doc_diff";
                    t["description"] = QStringLiteral(
                        "Cross-doc corroboration filter. Reads "
                        "*.md files from `<project>/<reports_dir>/` "
                        "(64 KiB truncate per file, top-level only) "
                        "and returns findings cited by >= min_lanes "
                        "distinct reports at the same (file, line). "
                        "Pure regex pass; no LLM. Mirrors "
                        "indie_review_corroborate's disk path "
                        "(ANTS-1282) — the inline-reports alternative "
                        "is intentionally absent for cold-eyes. "
                        "Required: reports_dir. Optional: min_lanes "
                        "(default 2).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject rdProp;
                    rdProp["type"] = "string";
                    rdProp["description"] = QStringLiteral(
                        "Project-relative path to a directory of "
                        "*.md report files. Lane name = filename "
                        "stem. Top level only; sub-dirs not "
                        "recursed.");
                    QJsonObject mlProp;
                    mlProp["type"]    = "integer";
                    mlProp["default"] = 2;
                    mlProp["minimum"] = 1;
                    mlProp["description"] = QStringLiteral(
                        "Minimum distinct lanes citing a (file, "
                        "line) for it to count as corroborated.");
                    QJsonObject props;
                    props["reports_dir"] = rdProp;
                    props["min_lanes"]   = mlProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("reports_dir");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                {
                    QJsonObject t;
                    t["name"] = "cold_eyes_fold_in";
                    t["description"] = QStringLiteral(
                        "Render an `### 📝 Cold-eyes <YYYY-MM-DD>` "
                        "ROADMAP block from a list of corroborated "
                        "findings. Allocates IDs from "
                        ".roadmap-counter (via "
                        "RoadmapFoldIn::allocateIds) and, if a "
                        "release-block heading is found via "
                        "findActiveReleaseHeading, atomically "
                        "inserts the block into ROADMAP.md. "
                        "Required: actionable (array).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject aProp;
                    aProp["type"] = "array";
                    aProp["description"] = QStringLiteral(
                        "Array of {file, line, citing_lanes[]} "
                        "objects describing the corroborated set.");
                    QJsonObject dProp;
                    dProp["type"] = "string";
                    dProp["description"] = QStringLiteral(
                        "ISO date for the heading + Source. "
                        "Defaults to today.");
                    QJsonObject hProp;
                    hProp["type"] = "string";
                    hProp["description"] = QStringLiteral(
                        "Optional explicit `## ` heading to "
                        "insert after; defaults to "
                        "RoadmapFoldIn::findActiveReleaseHeading.");
                    QJsonObject props;
                    props["actionable"]            = aProp;
                    props["date_iso"]              = dProp;
                    props["release_block_heading"] = hProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("actionable");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1283 — session_memory KV. Per-cwd key-value
                // persistence backed by
                // ~/.cache/ants-terminal/mcp-state/<cwd-hash>.json.
                // Pure delegation to SessionMemoryEngine::execute.
                {
                    QJsonObject t;
                    t["name"] = "session_memory";
                    t["description"] = QStringLiteral(
                        "Per-cwd key-value store the MCP server backs "
                        "to ~/.cache/ants-terminal/mcp-state/"
                        "<cwd-hash>.json. Use for cross-session "
                        "caches: last audit timestamp, partition "
                        "snapshots, tool-detection results. 100 KiB "
                        "cap per cwd; 16 KiB cap per value. Required: "
                        "op (\"get\"/\"set\"/\"delete\"/\"list\"). "
                        "key required for get/set/delete; value "
                        "required for set. See "
                        "docs/specs/ANTS-1283.md.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject opProp;
                    opProp["type"] = "string";
                    QJsonArray   opEnum;
                    opEnum.append(QStringLiteral("get"));
                    opEnum.append(QStringLiteral("set"));
                    opEnum.append(QStringLiteral("delete"));
                    opEnum.append(QStringLiteral("list"));
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral(
                        "Operation: get / set / delete / list.");
                    QJsonObject keyProp;
                    keyProp["type"] = "string";
                    keyProp["description"] = QStringLiteral(
                        "Key, ^[A-Za-z0-9._-]{1,64}$. Required for "
                        "get/set/delete.");
                    QJsonObject valProp;
                    // No type constraint — any JSON value accepted.
                    valProp["description"] = QStringLiteral(
                        "Any JSON value (≤ 16 KiB serialised). "
                        "Required for set.");
                    QJsonObject cwdProp;
                    cwdProp["type"] = "string";
                    cwdProp["description"] = QStringLiteral(
                        "Optional cwd override (default = focused "
                        "project root). Treated as a hash input, "
                        "not a permission check.");
                    QJsonObject props;
                    props["op"]    = opProp;
                    props["key"]   = keyProp;
                    props["value"] = valProp;
                    props["cwd"]   = cwdProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("op"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

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

                // ANTS-1284 — hoist args + responseText out of the
                // branches so a single recordCall covers both the
                // inline get_session_info path and registered providers.
                const QJsonObject argsObj = params.value("arguments").toObject();
                QString responseText;
                bool toolHandled = false;
                // ANTS-1253: get_session_info stays inline — it reads
                // ClaudeIntegration's own private state (m_state,
                // m_currentTool, m_contextPercent, m_changedFiles,
                // m_activeSessionId) rather than delegating to an
                // external provider. All 12 outward-delegate tools
                // dispatch via the registry below.
                if (toolName == "get_session_info") {
                    QJsonObject info;
                    info["state"] = static_cast<int>(m_state);
                    info["current_tool"] = m_currentTool;
                    info["context_percent"] = m_contextPercent;
                    info["changed_files"] = QJsonArray::fromStringList(m_changedFiles);
                    info["session_id"] = m_activeSessionId;
                    responseText = QString::fromUtf8(
                        QJsonDocument(info).toJson(QJsonDocument::Compact));
                    toolHandled = true;
                } else if (auto it = m_toolProviders.find(toolName);
                           it != m_toolProviders.end()) {
                    responseText = it->second(argsObj);
                    toolHandled = true;
                }

                if (toolHandled) {
                    // ANTS-1294 — frame user-supplied content as data,
                    // not instructions. Control-plane tools (server-
                    // generated state) skip the wrap so a caller can
                    // syntactically distinguish structural metadata
                    // from content. See docs/specs/ANTS-1294.md.
                    const bool isControlPlane =
                        (toolName == QStringLiteral("get_session_info") ||
                         toolName == QStringLiteral("token_usage"));
                    const QString wrapped = isControlPlane
                        ? responseText
                        : wrapMcpData(toolName, responseText);
                    result["content"] = makeTextContent(wrapped);
                    // ANTS-1284 — record dispatch for token_usage report.
                    // Byte counts measure the wrapped payload (what
                    // actually crosses the wire).
                    const qint64 argBytes = QJsonDocument(argsObj)
                        .toJson(QJsonDocument::Compact).size();
                    const qint64 outBytes = wrapped.toUtf8().size();
                    m_tokenUsage.recordCall(toolName, argBytes, outBytes);
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

// --- ANTS-1294 — MCP output sanitisation ---

QString ClaudeIntegration::wrapMcpData(const QString &toolName,
                                       const QString &payload) {
    // Neutralise close-tag breakout: a hostile commit message / file
    // line / scrollback chunk that contains the literal substring
    // </ants_mcp_data> would otherwise close the outer wrap and let
    // anything that follows be interpreted by the consuming assistant
    // as outside-the-wrap prose. Replace with a self-closing sentinel
    // of an unused tag name — visibly modified (a reviewer can tell
    // sanitisation happened) but inert.
    QString sanitised = payload;
    sanitised.replace(QStringLiteral("</ants_mcp_data>"),
                      QStringLiteral("<ants_mcp_data_escaped/>"));
    // Indie-review-2026-05-14 lane-5 ME-2: case/whitespace variants
    // (e.g. `</ANTS_MCP_DATA>`, `</ ants_mcp_data >`) — some assistant
    // tokenisers normalise tag casing and whitespace before pattern
    // matching, so the strict-case sentinel from above isn't enough.
    // QRegularExpression i-flag + tolerant whitespace.
    static const QRegularExpression closeTagVariantRe(
        QStringLiteral(R"(</\s*ants_mcp_data\s*>)"),
        QRegularExpression::CaseInsensitiveOption);
    sanitised.replace(closeTagVariantRe,
                      QStringLiteral("<ants_mcp_data_escaped/>"));
    // Indie-review-2026-05-14 lane-3 M1: defensive escaping of the
    // tool-name attribute. The current registry guarantees the name
    // matches `^[a-z][a-z0-9_]+$` so this is paranoia today, but the
    // helper is public-static and reachable from any future caller;
    // a `"` / `>` / `<` / `&` slipping through would break the wrap.
    QString safeTool = toolName;
    safeTool.replace(QLatin1Char('&'),  QStringLiteral("&amp;"));
    safeTool.replace(QLatin1Char('<'),  QStringLiteral("&lt;"));
    safeTool.replace(QLatin1Char('>'),  QStringLiteral("&gt;"));
    safeTool.replace(QLatin1Char('"'),  QStringLiteral("&quot;"));
    return QStringLiteral("<ants_mcp_data tool=\"%1\">%2</ants_mcp_data>")
        .arg(safeTool, sanitised);
}

// --- Project / Session Discovery ---

QString ClaudeIntegration::decodeProjectPath(const QString &encoded) {
    // Claude Code encodes absolute project paths by replacing `/` with `-`.
    // The encoding is lossy: a leaf named `my-project` collides with the
    // two-segment path `my/project`. The preferred source of truth is the
    // `cwd` field inside the JSONL transcript (extractCwdFromTranscript) —
    // this function is the last-resort fallback for when no transcript is
    // available.
    //
    // Strategy: greedy left-to-right walk that probes the filesystem.
    // At each hyphen we check which form (`/` vs embedded `-`) points at
    // something that exists on disk and prefer that. When neither candidate
    // exists we default to `/` (matches the legacy behavior for paths
    // without embedded hyphens, so well-formed cases don't regress).
    if (!encoded.startsWith('-')) return encoded;
    const QStringList tokens = encoded.mid(1).split('-');
    if (tokens.isEmpty() || tokens.first().isEmpty()) return QStringLiteral("/");

    QString path = QLatin1Char('/') + tokens.first();
    for (int i = 1; i < tokens.size(); ++i) {
        const QString withSep = path + QLatin1Char('/') + tokens[i];
        const QString withHyphen = path + QLatin1Char('-') + tokens[i];
        if (QFileInfo::exists(withSep))      path = withSep;
        else if (QFileInfo::exists(withHyphen)) path = withHyphen;
        else                                 path = withSep;
    }
    return path;
}

QString ClaudeIntegration::encodeProjectPath(const QString &path) {
    // Matches Claude Code's encoding: BOTH `/` AND `_` collapse to `-`.
    // ANTS-1192 root-cause for the chip + BG-tasks button hiding after
    // a project-directory rename like Ants-Terminal → Ants_Terminal:
    // Claude Code stores sessions under `~/.claude/projects/<encoded>/`
    // where it folds underscores into hyphens, so a cwd
    // `/mnt/Storage/Scripts/Linux/Ants_Terminal` lives at
    // `-mnt-Storage-Scripts-Linux-Ants-Terminal` on disk. We were
    // producing `-mnt-Storage-Scripts-Linux-Ants_Terminal` (underscore
    // preserved) which never matched, so QDir::exists() failed and
    // sessionPathForCwd silently returned empty.
    QString encoded = path;
    encoded.replace('/', '-');
    encoded.replace('_', '-');
    return encoded;
}

// Extract the real project path from a transcript's first user message cwd field
static QString extractCwdFromTranscript(const QString &transcriptPath) {
    QFile file(transcriptPath);
    if (!file.open(QIODevice::ReadOnly)) return {};

    // 0.7.52 (2026-04-27 indie-review HIGH) — cap readLine at 64 KiB. The
    // transcript is a JSONL file written by Claude Code; pathological /
    // corrupted input could put a multi-GiB single line at the head and
    // OOM the process before we get to the early-return cap. 64 KiB is
    // generous: real transcript records are <2 KiB, and any object whose
    // serialized form exceeds 64 KiB has no `cwd` field worth recovering.
    constexpr qint64 kMaxLineBytes = 64 * 1024;

    int linesRead = 0;
    while (!file.atEnd() && linesRead < 30) {
        QByteArray line = file.readLine(kMaxLineBytes).trimmed();
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
    QDir projectsDir(ConfigPaths::claudeProjectsDir());
    if (!projectsDir.exists()) return projects;

    // Load active session metadata to mark active sessions
    // Also build sessionId -> name + cwd map
    QSet<QString> activeSessionIds;
    QHash<QString, QString> sessionNames;   // sessionId -> name
    QHash<QString, QString> sessionCwds;    // sessionId -> cwd
    QDir sessionsDir(ConfigPaths::claudeSessionsDir());
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
    QFile file(ConfigPaths::claudeProjectMemory(projectEncoded));
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

