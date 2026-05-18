#include "claudeintegration.h"

#include "configpaths.h"
#include "debuglog.h"
#include "secureio.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QCoreApplication>

#include <climits>
#include <limits>
#include <utility>
#include <sys/socket.h>

// ANTS-1356 — monotonic clock used by rateLimitCheck in production.
// Started once in the ClaudeIntegration constructor; tests bypass via
// the synthetic-nowMs parameter so this clock is never read during
// pure-function tests.
static QElapsedTimer s_rateLimitClock;
#include <unistd.h>

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
    // ANTS-1427 — wrap every handler with a lambda-entry log line so
    // multi-checkpoint debugging sees: lambda-enter (here) → cmd*
    // body checkpoint (per-cmd one-liner) → recordDispatch (final).
    // Zero overhead in production (single bit-test when category off).
    ToolHandler wrapped =
        [name, inner = std::move(handler)]
        (const QJsonObject &args) -> QString {
            ANTS_LOG(DebugLog::Claude,
                     "mcp lambda-enter tool=%s arg_keys=%lld",
                     name.toUtf8().constData(),
                     static_cast<long long>(args.size()));
            return inner(args);
        };
    m_toolProviders[name] = std::move(wrapped);
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
    o["args_sha16"]  = r.argsSha16;
    o["resp_bytes"]  = r.respBytes;
    o["duration_us"] = r.durationUs;
    o["cache_hit"]   = r.cacheHit;
    o["result"]      = r.result;
    return o;
}

void ClaudeIntegration::recordMcpTrace(
    const QString &toolName, const QJsonObject &args,
    qint64 argBytes, qint64 respBytes, qint64 durationUs,
    bool cacheHit, const QString &result) {
    // INV-5: mcp_trace never records itself.
    if (toolName == QLatin1String("mcp_trace")) return;
    McpTraceRecord rec;
    rec.id         = m_mcpTraceNextId++;
    rec.tsMs       = QDateTime::currentMSecsSinceEpoch();
    rec.tool       = toolName;
    rec.argKeys    = argShapeOf(args);
    rec.argBytes   = argBytes;
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

// ANTS-1402-INV-2 — single dispatch-observation hook. Tees the
// same numbers to m_tokenUsage and recordMcpTrace.
// ANTS-1432 — recordCall now fires on every dispatch with a
// `success` flag derived from `result`. The pre-1432 short-circuit
// (recordCall skipped when result != "ok") meant per-tool byte
// counters under-reported failed-call cost.
void ClaudeIntegration::recordDispatch(
    const QString &toolName, const QJsonObject &argsObj,
    qint64 argBytes, qint64 outBytes, qint64 wrapBytes,
    qint64 durUs, bool cachedHit, const QString &result) {
    // ANTS-1432 — recordCall now fires on every dispatch. The engine
    // routes the byte counts into success- or failed-* accumulators
    // based on `success`. Pre-1432 behaviour skipped failed branches
    // entirely; that masked waste-on-failure cost (Vestige CC's
    // 2026-05-16 observation: "MCP cost tokens for the failed query
    // and saved none").
    const bool succeeded = (result == QLatin1String("ok"));
    m_tokenUsage.recordCall(toolName, argBytes, outBytes,
                            wrapBytes, durUs, succeeded);
    recordMcpTrace(toolName, argsObj, argBytes, outBytes,
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

                // ANTS-1520 — shared schema property for tools that
                // anchor on caller_cwd. Most project-scoped read and
                // write tools now REQUIRE this (refused at the
                // dispatcher with code `caller_cwd_required` when
                // absent). The five terminal-state verbs
                // (get_scrollback, get_text, get_last_command,
                // get_environment, get_cwd) classified TabSpecific
                // still anchor here when present but fall back to
                // the focused Ants tab when absent — for those tools
                // it remains a routing hint, not a refusal gate.
                // Uniform schema opener so consumers see one
                // canonical statement instead of N variants
                // (ANTS-1520 spec: "per-tool docstring opens with
                // the same line"). Original ANTS-1391 rationale
                // (don't return project A's data when a session is
                // asking about project B) drove the uniform-Required
                // upgrade.
                auto makeCallerCwdReadProp = []{
                    QJsonObject p;
                    p["type"] = "string";
                    p["description"] = QStringLiteral(
                        "Your $PWD. Required by the dispatcher for "
                        "project-scoped read and write tools "
                        "(refuses with code `caller_cwd_required` "
                        "when absent — ANTS-1520). The terminal-"
                        "state verbs (`get_*`) still accept it as "
                        "an Optional tab-routing anchor and fall "
                        "back to the focused Ants tab when absent.");
                    return p;
                };

                // ANTS-1409 — canonical "Pass caller_cwd to anchor"
                // suffix used by three tool descriptions verbatim
                // (get_last_command, get_git_status, get_environment).
                // The helper keeps the phrasing in one place so
                // future MCP tools opt in via `+ callerCwdSuffix()`
                // rather than re-typing it. INV-4 of the spec
                // explicitly excludes get_scrollback and get_text —
                // those carry tool-specific phrasing the canonical
                // short suffix doesn't.
                auto callerCwdSuffix = []{
                    return QStringLiteral(
                        "Pass `caller_cwd` to anchor to your tab (ANTS-1392).");
                };

                // ANTS-1499 — `etag_match` input prop for read tools
                // that opt into the "304 Not Modified" pattern. Eight
                // tools allowlisted in isEtagSupportedTool(); the same
                // schema fragment is reused for each so the field
                // description stays in lockstep.
                auto makeEtagMatchProp = []{
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
                        "Modified\" pattern).");
                    return p;
                };

                QJsonObject scrollbackTool;
                scrollbackTool["name"] = "get_scrollback";
                scrollbackTool["description"] = QStringLiteral(
                    "Get the last N lines of terminal scrollback. "
                    "Pass `caller_cwd` (your $PWD) to anchor to your "
                    "tab; without it the result comes from whichever "
                    "tab Ants happens to have focused (ANTS-1392). "
                    "ANTS-1500: pass `since_cursor` (from a prior "
                    "response) for incremental-fetch mode — server "
                    "returns only the bytes appended since the cursor "
                    "(envelope: {ok, content, cursor, cursor_stale, "
                    "stale_reason?}) instead of the full window. "
                    "Saves 80-95% on rapid-polling loops. Stale "
                    "cursors (ring wrap, terminal restart) flip "
                    "cursor_stale:true and fall back to the full "
                    "window.");
                // ANTS-1453 — selection_hint: one-sentence
                // form-factor cue for the calling assistant.
                scrollbackTool["selection_hint"] = QStringLiteral(
                    "Use when you need recent terminal output (e.g. "
                    "the last build error). Prefer over Read/Grep "
                    "when the data lives in stdout, not on disk. "
                    "Polling loops: pass since_cursor for delta-only "
                    "responses (ANTS-1500).");
                // ANTS-1395 — scope props/schema inside a block so every
                // subsequent tool's matching declarations (each already
                // wrapped in `{ ... }`) no longer trigger
                // -Wshadow=compatible-local against this outer-scope pair.
                {
                    QJsonObject linesParam;
                    linesParam["type"] = "integer";
                    linesParam["default"] = 50;
                    QJsonObject props;
                    props["lines"] = linesParam;
                    // ANTS-1392 — caller_cwd anchor for the terminal-state
                    // verbs. Optional; falls back to focused tab when absent.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    // ANTS-1500 — since_cursor opt-in for incremental
                    // mode. Absent → legacy raw-text return. Present →
                    // JSON envelope with content+cursor+stale flag.
                    QJsonObject sinceProp;
                    sinceProp["type"] = "string";
                    sinceProp["description"] = QStringLiteral(
                        "Optional. Opaque cursor token from a prior "
                        "response. When present, the server returns "
                        "only content appended since the cursor was "
                        "issued; cursor_stale:true falls back to the "
                        "full window (terminal restart, ring wrap). "
                        "Triggers JSON-envelope response shape; absent "
                        "preserves the legacy raw-text return.");
                    props["since_cursor"] = sinceProp;
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["properties"] = props;
                    scrollbackTool["inputSchema"] = schema;
                }
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
                cwdTool["description"] = QStringLiteral(
                    "Get a terminal cwd. If `caller_cwd` is passed "
                    "(your $PWD), the server echoes it back canonical "
                    "(ANTS-1391); otherwise returns the focused tab's "
                    "shellCwd, which may NOT be your tab in a multi-"
                    "Ants-tab setup. Pass caller_cwd to avoid the "
                    "cross-tab leak.");
                cwdTool["selection_hint"] = QStringLiteral(
                    "Use when you need the canonical form of your "
                    "$PWD (multi-tab disambiguation, symlink resolve). "
                    "Cheap (~100 B).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Optional. Your $PWD. When present, echoed "
                        "back canonical instead of returning the "
                        "focused-tab cwd.");
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    cwdTool["inputSchema"] = schema;
                }
                tools.append(cwdTool);

                QJsonObject sessionTool;
                sessionTool["name"] = "get_session_info";
                sessionTool["description"] = "Get terminal session metadata";
                sessionTool["selection_hint"] = QStringLiteral(
                    "Use once at session start to learn tab/process "
                    "layout. Control-plane (no caller_cwd anchor); "
                    "cheap.");
                sessionTool["inputSchema"] = emptySchema;
                tools.append(sessionTool);

                QJsonObject lastCmdTool;
                lastCmdTool["name"] = "get_last_command";
                lastCmdTool["description"] = QStringLiteral(
                    "Get the last command's exit code and output "
                    "(via shell integration). Optional `mode` — "
                    "\"full\" (default) returns {exit_code, output, "
                    "failed}; \"summary\" (ANTS-1503) returns "
                    "{exit_code, line_count, last_20[], ms, failed} "
                    "— skip the full body when you only need "
                    "exit + tail + duration. ") + callerCwdSuffix();
                lastCmdTool["selection_hint"] = QStringLiteral(
                    "Use after a shell command to inspect exit code "
                    "+ output without scraping scrollback. Requires "
                    "OSC 133 shell integration. Prefer mode:summary "
                    "for status checks (≤500 bytes vs full mode's "
                    "1-4 KiB).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    QJsonObject modeProp;
                    modeProp["type"] = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("full");
                    modeEnum.append("summary");
                    modeProp["enum"] = modeEnum;
                    modeProp["description"] = QStringLiteral(
                        "Response shape. \"full\" (default) — "
                        "{exit_code, output, failed}. \"summary\" — "
                        "{exit_code, line_count, last_20[], ms, "
                        "failed}.");
                    props["mode"] = modeProp;
                    schema["properties"] = props;
                    lastCmdTool["inputSchema"] = schema;
                }
                tools.append(lastCmdTool);

                QJsonObject gitTool;
                gitTool["name"] = "get_git_status";
                gitTool["description"] = QStringLiteral(
                    "Get git branch, status, and recent commits for "
                    "the terminal's CWD. ") + callerCwdSuffix();
                gitTool["selection_hint"] = QStringLiteral(
                    "Use for branch + dirty-state + recent-commits "
                    "in one call. Cheaper than spawning `git status` "
                    "yourself; prefer git_state for diff/log too.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    gitTool["inputSchema"] = schema;
                }
                tools.append(gitTool);

                QJsonObject envTool;
                envTool["name"] = "get_environment";
                envTool["description"] = QStringLiteral(
                    "Get shell environment info (PATH, virtualenv, "
                    "key env vars). ") + callerCwdSuffix();
                envTool["selection_hint"] = QStringLiteral(
                    "Use when env vars matter for the diagnosis "
                    "(PATH resolution, language venv, terminal "
                    "type). Read-only.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"] = props;
                    envTool["inputSchema"] = schema;
                }
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
                    "bullets. Each bullet: {id, status, headline, "
                    "headline_oneline, kind, lanes}. `headline_oneline` "
                    "(ANTS-1521) is `headline` with newlines + "
                    "whitespace runs collapsed to a single space — safe "
                    "to concatenate into a summary without post-"
                    "processing. Optional `include_body:true` "
                    "(ANTS-1517) adds a `body` field (truncated to "
                    "~2000 chars, `body_truncated:true` set on "
                    "truncation) — saves the 3-5 follow-up Reads a "
                    "session does to pick up Kind / Lanes / Source "
                    "prose from a dense bundle table. Optional "
                    "`status` filter — \"active\" "
                    "(📋+🚧, ~1.7 K tokens — recommended for planning "
                    "queries) / \"shipped\" (✅ only) / \"all\" (default, "
                    "~12 K tokens). Optional `section` slug — returns "
                    "only bullets within that ## or ### heading "
                    "(e.g. \"performance\", \"080\"); response carries "
                    "`section` echo. Optional `mode` — \"bullets\" "
                    "(default) / \"section_index\" (returns a compact "
                    "{slug, headline, level, active_count, "
                    "shipped_count, total_count}[] index instead of "
                    "bullets[] — use for slug discovery before drilling "
                    "in via section=). Envelope: {ok, bullets, path, "
                    "count, filter, section?, mode?} or "
                    "{ok, sections, path, filter, mode} for "
                    "section_index, or {ok:false, error, code} on error. "
                    "`bullets[].id` follows the shareable "
                    "docs/standards/roadmap-format.md § 3.5.1 spec — "
                    "any `[PROJ-NNNN]` token (letter-prefixed, "
                    "dash-then-digits) is recognised, e.g. "
                    "`[ANTS-1234]`, `[MAME-CURATOR-42]`, "
                    "`[mame-curator-7]` (ANTS-1405).");
                roadmapTool["selection_hint"] = QStringLiteral(
                    "Use when you need to know which roadmap items "
                    "are active/shipped before quoting an ID. "
                    "Prefer over Read for triage queries; use Read "
                    "for full-text edits.");
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
                        "for partial queries. Slugs are canonically "
                        "lowercase; off-case spelling → code=bad_case "
                        "with `canonical_slug` surfaced (ANTS-1524). "
                        "Unknown slug → code=bad_section.");
                    props["section"] = sectionProp;
                    // ANTS-1398 — opt-in to retain section-rollup
                    // bullets (empty id/headline, status emoji only).
                    // Default false; the dropped rollups are visual
                    // chrome the dialog uses but actionable-item
                    // clients never want them in bullets[].
                    QJsonObject inclHeadersProp;
                    inclHeadersProp["type"] = "boolean";
                    inclHeadersProp["default"] = false;
                    inclHeadersProp["description"] = QStringLiteral(
                        "If true, retain section-rollup bullets "
                        "(empty id+headline, status emoji only) in "
                        "bullets[]. Default false — drops them so "
                        "clients get only actionable entries. "
                        "Opt-in for back-compat callers (ANTS-1398).");
                    props["include_section_headers"] = inclHeadersProp;
                    // ANTS-1425 — opt-in for narrator bullets (empty
                    // id, non-empty headline — section-summary prose).
                    QJsonObject inclNarratorsProp;
                    inclNarratorsProp["type"] = "boolean";
                    inclNarratorsProp["default"] = false;
                    inclNarratorsProp["description"] = QStringLiteral(
                        "If true, retain narrator bullets (empty id, "
                        "non-empty headline — section-summary prose "
                        "like \"Trust-model gaps in IPC sockets.\"). "
                        "Default false — roadmap-format.md § 3.5.1 "
                        "makes the [PROJ-NNNN] ID mandatory for every "
                        "actionable bullet, so empty-id is a "
                        "non-actionable marker. Opt-in for back-compat "
                        "callers (ANTS-1425).");
                    props["include_narrator_bullets"] = inclNarratorsProp;
                    // ANTS-1517 — include_body opt-in. Default false.
                    QJsonObject inclBodyProp;
                    inclBodyProp["type"] = "boolean";
                    inclBodyProp["default"] = false;
                    inclBodyProp["description"] = QStringLiteral(
                        "If true, each bullet carries a `body` field "
                        "(continuation prose, truncated to ~2000 chars "
                        "with `body_truncated:true` on truncation). "
                        "Default false. Use when triaging dense bundle "
                        "tables where the rationale lives in the body, "
                        "not the headline (ANTS-1517).");
                    props["include_body"] = inclBodyProp;
                    // ANTS-1437 — mode arg. Default "bullets" (legacy).
                    // "section_index" returns a compact section index
                    // instead of bullets — use to discover slugs cheaply.
                    QJsonObject modeProp;
                    modeProp["type"] = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("bullets");
                    modeEnum.append("section_index");
                    modeProp["enum"] = modeEnum;
                    modeProp["default"] = "bullets";
                    modeProp["description"] = QStringLiteral(
                        "Response mode. \"bullets\" (default) returns "
                        "bullets[]. \"section_index\" returns "
                        "sections[{slug, headline, level, active_count, "
                        "shipped_count, total_count}] and no bullets — "
                        "use for slug discovery (response < 5 KB on a "
                        "500-bullet roadmap). Cannot combine with "
                        "section= (bad_mode_combo).");
                    props["mode"] = modeProp;
                    // ANTS-1436 — offset/limit pagination args.
                    QJsonObject offsetProp;
                    offsetProp["type"] = "integer";
                    offsetProp["minimum"] = 0;
                    offsetProp["description"] = QStringLiteral(
                        "0-based start index into the post-filter "
                        "bullets array. Defaults to 0. Use with "
                        "`limit` to page through large responses. "
                        "Past-end returns empty bullets[] with "
                        "offset == total.");
                    props["offset"] = offsetProp;
                    QJsonObject limitProp;
                    limitProp["type"] = "integer";
                    limitProp["minimum"] = 1;
                    limitProp["maximum"] = 500;
                    limitProp["description"] = QStringLiteral(
                        "Cap on bullets[] length (1..500). If "
                        "omitted, server auto-truncates when the "
                        "response would exceed ~20 KB and emits "
                        "`truncated:true` + `next_offset`. Explicit "
                        "limit wins (auto-pick only fires when "
                        "omitted). When pagination applies, envelope "
                        "carries offset/limit/total/truncated and "
                        "next_offset when truncated.");
                    props["limit"] = limitProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"] = props;
                    roadmapTool["inputSchema"] = schema;
                }
                tools.append(roadmapTool);

                // ANTS-1583 — roadmap_branch_drift descriptor.
                {
                    QJsonObject t;
                    t["name"] = "roadmap_branch_drift";
                    t["description"] = QStringLiteral(
                        "Compare ROADMAP ✅ entries' cited commit SHAs "
                        "against HEAD's reachable history. Returns a "
                        "drift list when a claimed-shipped commit "
                        "isn't reachable from current HEAD — useful for "
                        "projects with multiple long-lived branches "
                        "where fix commits and docs commits land on "
                        "different branches and drift. Composes with "
                        "last_audit_summary's branch capture "
                        "(ANTS-1576) — once the SARIF records its "
                        "source branch, this verb's drift list tells "
                        "the caller whether to trust the audit's "
                        "claim. Use after a rebase / multi-branch "
                        "merge to validate ROADMAP claims against "
                        "actual code state. Envelope: {ok, "
                        "current_branch, current_commit, "
                        "scanned_bullets, with_sha, drift_count, "
                        "drift:[{bullet_id, cited_sha, reason, "
                        "headline}], path, drift_truncated?, "
                        "truncated_history?}. SHA detector uses an "
                        "anchored regex (commit-prefix or trailing-"
                        "punct context required) plus alpha-required "
                        "lookahead — keeps the false-positive rate "
                        "low on heterogeneous citation forms.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when investigating 'ROADMAP says ✅ but "
                        "the bug is still there', or before claiming "
                        "a fix shipped. Cross-checks against "
                        "last_audit_summary's branch/commit "
                        "(ANTS-1576).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject maxDriftProp;
                    maxDriftProp["type"]    = "integer";
                    maxDriftProp["default"] = 20;
                    maxDriftProp["minimum"] = 1;
                    maxDriftProp["maximum"] = 100;
                    maxDriftProp["description"] = QStringLiteral(
                        "Cap on drift[] length. Server-clamp [1, 100]. "
                        "When the scan hits the cap, "
                        "`drift_truncated:true` flags the envelope.");
                    props["max_drift"]  = maxDriftProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    schema["required"]   = QJsonArray{
                        QStringLiteral("caller_cwd")};
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                QJsonObject tabListTool;
                tabListTool["name"] = "tab_list";
                tabListTool["description"] = QStringLiteral(
                    "List all open terminal tabs in this Ants instance. "
                    "Each tab: {index, title, cwd, shell_pid, "
                    "claude_running, color}. Envelope: {ok:true, tabs:[…]}.");
                tabListTool["selection_hint"] = QStringLiteral(
                    "Use when multiple Ants tabs may exist and you "
                    "need to pick the right one (e.g. cross-tab "
                    "queries). Cheap; no caller_cwd anchor.");
                // ANTS-1499 — tab_list opts into the etag pattern so
                // a session polling for "did a tab appear?" pays the
                // full envelope only on the change. Schema carries
                // `etag_match` only — no other inputs.
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    tabListTool["inputSchema"] = schema;
                }
                tools.append(tabListTool);

                QJsonObject getTextTool;
                getTextTool["name"] = "get_text";
                getTextTool["description"] = QStringLiteral(
                    "Read trailing scrollback lines from a tab. Optional "
                    "`tab` (explicit index), `caller_cwd` (your $PWD — "
                    "ANTS-1392, anchors to your tab when `tab` is "
                    "omitted), and `lines` (default 100, capped at "
                    "10000). Returns {ok, text, lines, bytes} or "
                    "{ok:false, error} when the tab index is out of "
                    "range.");
                getTextTool["selection_hint"] = QStringLiteral(
                    "Use when you need a specific tab's trailing "
                    "scrollback (not your own). Prefer get_scrollback "
                    "for your own tab; this one targets `tab` index.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject tabProp;   tabProp["type"]   = "integer";
                    QJsonObject linesProp; linesProp["type"] = "integer";
                                           linesProp["default"] = 100;
                    props["tab"]   = tabProp;
                    props["lines"] = linesProp;
                    // ANTS-1392 — caller_cwd anchor for the no-`tab`
                    // path. Falls back to focused tab when absent.
                    props["caller_cwd"] = makeCallerCwdReadProp();
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
                    "line, text, also_at?:[{file,line}…]}], truncated, "
                    "dedup, dedup_collapsed, respect_gitignore, "
                    "include_hidden, timeout_sec, elapsed_ms}. Prefer "
                    "this over `Bash grep -r ...` — typically saves "
                    "250-4500 tokens per query and avoids round-trips "
                    "for no-match cases. Args: pattern (required), "
                    "regex (false), lane (subdir under project root), "
                    "glob, max_results (default 50, cap 500), context, "
                    "case (smart/sensitive/insensitive), "
                    "respect_gitignore (default true — pass false for "
                    "stale-path audits across build outputs / "
                    "compile_commands.json / cache dirs), "
                    "include_hidden (default false — pass true to "
                    "search dotfile paths; .git/ stays excluded "
                    "regardless), dedup (default true — collapses "
                    "near-duplicate excerpts into a single primary "
                    "match with `also_at` carrying the rest; "
                    "ANTS-1501), timeout_sec (default 5, range [1,30] "
                    "— raise for mid-size projects > 2 k files; "
                    "ANTS-1565). On hard-kill the rg_failed envelope "
                    "carries a `hint` field with the three viable "
                    "next steps.");
                wsTool["selection_hint"] = QStringLiteral(
                    "Use for vague-location queries ('where is the X "
                    "feature wired up?'). For known one-keyword bug "
                    "hunts, 3 Grep calls can still be cheaper.");
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
                    // ANTS-1452 — opt-ins for gitignored / hidden files.
                    // Both default to pre-1452 behaviour so existing
                    // callers are unaffected; the description points
                    // Claude at the stale-path-audit / cache-dir use
                    // case for `respect_gitignore=false`.
                    QJsonObject respectGitignoreProp;
                    respectGitignoreProp["type"]    = "boolean";
                    respectGitignoreProp["default"] = true;
                    respectGitignoreProp["description"] = QStringLiteral(
                        "When false, bypass .gitignore/.ignore "
                        "(--no-ignore-vcs --no-ignore). Use for "
                        "audits across generated files like "
                        "compile_commands.json, build/, or cache "
                        "dirs.");
                    QJsonObject includeHiddenProp;
                    includeHiddenProp["type"]    = "boolean";
                    includeHiddenProp["default"] = false;
                    includeHiddenProp["description"] = QStringLiteral(
                        "When true, search dotfile paths (--hidden). "
                        "Excludes .git/ itself regardless of this "
                        "flag.");
                    // ANTS-1501 — near-duplicate excerpt dedup. Default
                    // on; collapses identical excerpts into one primary
                    // match with `also_at: [{file, line}, …]` carrying
                    // the rest. Pass dedup:false on the rare query
                    // where the exact surrounding context per hit
                    // matters more than per-call token cost.
                    QJsonObject dedupProp;
                    dedupProp["type"]    = "boolean";
                    dedupProp["default"] = true;
                    dedupProp["description"] = QStringLiteral(
                        "When true (default), group identical "
                        "whitespace-normalised excerpts into one "
                        "primary match plus `also_at:[{file,line}, …]` "
                        "for the duplicates. Pass false to keep the "
                        "per-match verbatim output.");
                    // ANTS-1565 — per-call wall-clock budget. Default
                    // 5 s (raised from 2 s); accept integer in [1, 30].
                    // Out-of-range values fall back to the default. The
                    // effective value is echoed on every response so
                    // callers can see what they got.
                    QJsonObject timeoutSecProp;
                    timeoutSecProp["type"]    = "integer";
                    timeoutSecProp["default"] = 5;
                    timeoutSecProp["minimum"] = 1;
                    timeoutSecProp["maximum"] = 30;
                    timeoutSecProp["description"] = QStringLiteral(
                        "Wall-clock budget for the rg invocation in "
                        "seconds. Clamped to [1, 30] (out-of-range "
                        "falls back to default). Raise when a "
                        "mid-size project (> 2 k files) hard-kills "
                        "the default; fall back to `Bash rg` for "
                        "queries that need longer than 30 s.");
                    props["pattern"]     = patternProp;
                    props["regex"]       = regexProp;
                    props["lane"]        = laneProp;
                    props["glob"]        = globProp;
                    props["max_results"] = maxProp;
                    props["context"]     = ctxProp;
                    props["case"]        = caseProp;
                    props["respect_gitignore"] = respectGitignoreProp;
                    props["include_hidden"]    = includeHiddenProp;
                    props["dedup"]             = dedupProp;
                    props["timeout_sec"]       = timeoutSecProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]  = makeCallerCwdReadProp();
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
                foTool["selection_hint"] = QStringLiteral(
                    "Use to map a large file's symbols before Read. "
                    "Prefer over `Read` when you only need a "
                    "structural overview (which class/function is "
                    "where).");
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
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]           = makeCallerCwdReadProp();
                    props["etag_match"]           = makeEtagMatchProp();   // ANTS-1499
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
                    "call vs Bash. op=\"status\" envelope (ANTS-1522): "
                    "`files[]` now includes untracked paths with "
                    "`index:\"?\"` + `worktree:\"?\"` for `git status "
                    "--porcelain` parity — one array, one shape. "
                    "`untracked[]` (DEPRECATED) is still emitted in "
                    "parallel for one release; removed in 0.7.93.");
                gsTool["selection_hint"] = QStringLiteral(
                    "Use for git status/log/diff in one structured "
                    "call (vs three Bash invocations). Pairs with "
                    "verify_changes after Edit/Write.");
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
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
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
                ssTool["selection_hint"] = QStringLiteral(
                    "Use as the first call on a 'where does feature "
                    "X live?' question — walks CLAUDE.md module-map. "
                    "Collapses 3-5 grep rounds into one.");
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
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
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
                    "Read the latest audit summary under "
                    "{cwd}/.audit_cache and return a compact envelope: "
                    "counts (error/warning/note/suppressed) plus "
                    "top_findings (sorted by level desc, confidence "
                    "desc, file asc, line asc). Saves ~5-15 K tokens "
                    "vs reading the HTML report. Discovery order: "
                    "audit-*.sarif → cppcheck-*.xml (ANTS-1459) → "
                    "clang-tidy-*.txt → semgrep-*.json (ANTS-1494). "
                    "Returns {ok:false, code:\"not_audited\"} if no "
                    "recognised report is present. "
                    "Provenance (ANTS-1545): scrape-on-query against "
                    "disk artefacts that a previous `audit_run` (or "
                    "an out-of-band AuditDialog sweep / CI job) wrote "
                    "to .audit_cache. NOT paired with any specific "
                    "writer call — every invocation re-opens and "
                    "re-parses the newest matching file. If you need "
                    "a fresh sweep, call `audit_run` first; this tool "
                    "never re-runs the scanners. "
                    "Cppcheck gotcha: the default `--check-level=normal` "
                    "branch budget can silently truncate findings on "
                    "files > 5 K LoC; re-run cppcheck with "
                    "--check-level=exhaustive for full coverage.");
                lasTool["selection_hint"] = QStringLiteral(
                    "Use when planning audit-touching work to see "
                    "what's already been flagged. Cheap; runs against "
                    "cached SARIF — no re-scan.");
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
                    // ANTS-1540 — `rule_ids: ["check-id-1", ...]`.
                    // Optional. When set, top_findings[] is filtered
                    // to only entries whose ruleId is in the list.
                    // Server expands the internal cap to 50 then
                    // re-applies top_n on the filtered slice so a
                    // rare rule below the default top-5 ranking
                    // still surfaces. Counts[] stays global.
                    QJsonObject ruleIdsProp;
                    ruleIdsProp["type"] = "array";
                    QJsonObject ruleIdsItems;
                    ruleIdsItems["type"] = "string";
                    ruleIdsProp["items"] = ruleIdsItems;
                    ruleIdsProp["description"] = QStringLiteral(
                        "Optional. Filter top_findings[] to only "
                        "those with ruleId in this list. Server "
                        "scans up to 50 raw findings under the hood "
                        "so a rare rule below the default top-5 "
                        "ranking still appears. Counts (error / "
                        "warning / note / suppressed) stay global. "
                        "Empty array equivalent to absent.");
                    props["top_n"]          = topNProp;
                    props["severity_floor"] = floorProp;
                    props["rule_ids"]       = ruleIdsProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]     = makeCallerCwdReadProp();
                    props["etag_match"]     = makeEtagMatchProp();   // ANTS-1499
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
                    t["selection_hint"] = QStringLiteral(
                        "Use to split source files for multi-reviewer "
                        "indie-review dispatch. Pairs with "
                        "indie_review_brief + indie_review_dispatch.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
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
                    t["selection_hint"] = QStringLiteral(
                        "Use to assemble the brief for one "
                        "indie-review chunk. Run after "
                        "indie_review_partition.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject laneProp;
                    laneProp["type"] = "string";
                    laneProp["description"] = QStringLiteral(
                        "Lane name as returned by indie_review_partition.");
                    QJsonObject props;
                    props["lane"] = laneProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
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
                    t["selection_hint"] = QStringLiteral(
                        "Use to cross-check two+ reviewers' findings "
                        "against shared evidence. Reduces false "
                        "positives in N-reviewer fan-out.");
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
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]  = makeCallerCwdReadProp();
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
                    t["selection_hint"] = QStringLiteral(
                        "Use to draft the synthesis prompt for "
                        "folding N reviewer chunks into one report. "
                        "Run before the synthesis subagent dispatch.");
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
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
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
                        "actionable (array), caller_cwd (string — "
                        "your $PWD; ANTS-1372 cross-project gate).");
                    t["selection_hint"] = QStringLiteral(
                        "Use to merge a finished indie-review report "
                        "back into ROADMAP.md as a fold-in block. "
                        "Mutates ROADMAP — caller_cwd required.");
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
                    // ANTS-1389 — surface the ANTS-1372 caller-cwd gate
                    // in the schema so Claude Code's MCP client fills
                    // it on the first call.
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on mismatch "
                        "with the focused tab's cwd (ANTS-1372 "
                        "cross-project write gate).");
                    QJsonObject props;
                    props["actionable"]             = actProp;
                    props["date_iso"]               = dateProp;
                    props["release_block_heading"]  = hdrProp;
                    props["caller_cwd"]             = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("actionable");
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1352 — indie_review_dispatch (server-side
                // reviewer fan-out).
                {
                    QJsonObject t;
                    t["name"] = "indie_review_dispatch";
                    t["description"] = QStringLiteral(
                        "Server-side multi-agent independent code "
                        "review: dispatches N parallel HTTP POSTs to "
                        "the project's configured AI endpoint "
                        "(Settings → AI), one per lane from "
                        "indie_review_partition, saves each response "
                        "as <reports_dir>/<lane>.md, returns a "
                        "manifest. Replaces the per-Agent fan-out in "
                        "the /indie-review skill — displaces the "
                        "orchestration token tax onto the upstream "
                        "provider's billing. Required: caller_cwd, "
                        "reports_dir. Optional: lanes (subset of "
                        "partition; default all), concurrency [1-8] "
                        "default 4, max_tokens [4096-128000] default "
                        "64000, model (default = Config::aiModel(), "
                        "which is \"llama3\" out-of-box — see "
                        "docs/specs/ANTS-1352.md § 3.3), "
                        "system_extras (≤ 4 KiB append to system "
                        "prompt). See docs/specs/ANTS-1352.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use as the entry-point orchestrator for an "
                        "end-to-end indie-review run. Wraps "
                        "partition → brief → dispatch → corroborate.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Anchors lane partition lookup "
                        "and reports_dir. caller_cwd Required "
                        "(ANTS-1404).");
                    QJsonObject rdProp;
                    rdProp["type"] = "string";
                    rdProp["description"] = QStringLiteral(
                        "Project-relative directory under caller_cwd "
                        "where <lane>.md files are written. Created "
                        "if missing. Refuses on collision (delete "
                        "stale files or pick a fresh dir).");
                    QJsonObject lanesProp;
                    lanesProp["type"] = "array";
                    lanesProp["description"] = QStringLiteral(
                        "Optional subset of lane names from "
                        "indie_review_partition. Empty = all.");
                    QJsonObject concProp;
                    concProp["type"] = "integer";
                    concProp["description"] = QStringLiteral(
                        "Max parallel in-flight requests, [1, 8]. "
                        "Default 4.");
                    QJsonObject mtProp;
                    mtProp["type"] = "integer";
                    mtProp["description"] = QStringLiteral(
                        "Per-lane response cap forwarded to upstream "
                        "API, [4096, 128000]. Default 64000.");
                    QJsonObject modelProp;
                    modelProp["type"] = "string";
                    modelProp["description"] = QStringLiteral(
                        "Model name passed verbatim to upstream. "
                        "\"auto\" or empty → Config::aiModel().");
                    QJsonObject extrasProp;
                    extrasProp["type"] = "string";
                    extrasProp["description"] = QStringLiteral(
                        "Appended to system prompt with a `---` "
                        "separator. Useful for project-specific "
                        "threat-model hints. ≤ 4 KiB.");
                    QJsonObject props;
                    props["caller_cwd"]    = callerProp;
                    props["reports_dir"]   = rdProp;
                    props["lanes"]         = lanesProp;
                    props["concurrency"]   = concProp;
                    props["max_tokens"]    = mtProp;
                    props["model"]         = modelProp;
                    props["system_extras"] = extrasProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("reports_dir");
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
                    t["selection_hint"] = QStringLiteral(
                        "Use when planning a debt-sweep pass. Returns "
                        "triaged findings + suggested fixes; pairs "
                        "with debt_sweep_apply_fix + _defer.");
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
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
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
                        "no-op (file_changed / not_fixable). Required: "
                        "caller_cwd (string — your $PWD; ANTS-1372).");
                    t["selection_hint"] = QStringLiteral(
                        "Use to apply ONE triaged debt-sweep fix in "
                        "place. Mutates files — caller_cwd Required. "
                        "Pairs with debt_sweep_scan.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject didProp; didProp["type"] = "string";
                    QJsonObject fProp;   fProp["type"]   = "string";
                    QJsonObject lProp;   lProp["type"]   = "integer";
                    QJsonObject aProp;   aProp["type"]   = "boolean";
                    aProp["description"] = QStringLiteral(
                        "Caller asserts the finding was auto_fixable in "
                        "the prior scan. Defaults to true.");
                    // ANTS-1389 — caller_cwd schema surfacing.
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on mismatch "
                        "with the focused tab's cwd (ANTS-1372).");
                    QJsonObject props;
                    props["detector_id"]  = didProp;
                    props["file"]         = fProp;
                    props["line"]         = lProp;
                    props["auto_fixable"] = aProp;
                    props["caller_cwd"]   = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("detector_id");
                    req.append("file");
                    req.append("line");
                    req.append("caller_cwd");
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
                        "deferred (array), caller_cwd (string — your "
                        "$PWD; ANTS-1372).");
                    t["selection_hint"] = QStringLiteral(
                        "Use to defer (not action) a debt-sweep "
                        "finding set so it doesn't re-surface every "
                        "scan. Mutates ROADMAP — caller_cwd required.");
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
                    // ANTS-1389 — caller_cwd schema surfacing.
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on mismatch "
                        "with the focused tab's cwd (ANTS-1372).");
                    QJsonObject props;
                    props["deferred"]              = dProp;
                    props["date_iso"]              = dateProp;
                    props["release_block_heading"] = hdrProp;
                    props["caller_cwd"]            = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("deferred");
                    req.append("caller_cwd");
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
                    t["selection_hint"] = QStringLiteral(
                        "Use to draft the triage prompt for the "
                        "judgment-required subset of debt-sweep "
                        "findings. Pure templating; no LLM call.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject fProp; fProp["type"] = "array";
                    fProp["description"] = QStringLiteral(
                        "Array of Finding-shaped objects.");
                    QJsonObject props;
                    props["findings"] = fProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
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
                        "ANTS-1359 — results are cached in-process for "
                        "up to 5 minutes keyed on git state + options; "
                        "responses carry cache_hit. Pass force_refresh "
                        "to bypass; pass cache_only to probe without "
                        "running. Required: caller_cwd (string — your "
                        "$PWD; ANTS-1372 cross-project gate). "
                        "**Timeouts are two-tier (ANTS-1525/1579):** "
                        "the **tool-side** per-gate budget kills the "
                        "gate process and surfaces a structured "
                        "envelope (`tool_timed_out:true` + "
                        "`timed_out_gate` + `per_gate_timeout_sec` + "
                        "`timeout_hint`). The **transport-side** cap "
                        "is the MCP client's request timer (Claude "
                        "Code's is ~60 s) — when that fires, it "
                        "closes the socket before any envelope can "
                        "arrive and the caller sees `MCP error "
                        "-32000: transport: timed out` *outside* the "
                        "response. For builds > 60 s, fall back to "
                        "`Bash → cmake/make` rather than raising "
                        "`timeout_sec`; this tool's [10, 1800] clamp "
                        "is independent of the transport cap.");
                    t["selection_hint"] = QStringLiteral(
                        "Use after Edit/Write to verify build / "
                        "tests / lint gates still pass. Cheap "
                        "regression-prevention step; 5-min cache.");
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
                        "evenly across the gates that will actually "
                        "run (post-`gates` filter, ANTS-1492). Server-"
                        "clamped [10, 1800]; default 1200. Caveat: "
                        "most MCP clients (incl. Claude Code) apply "
                        "their own request-level transport timeout "
                        "independent of this budget — if your build "
                        "exceeds ~60 s and you see `MCP error -32000: "
                        "transport: timed out`, that is the client "
                        "closing the connection, not the tool. Fall "
                        "back to running the same command via Bash "
                        "for long builds.");
                    // ANTS-1389 — caller_cwd schema surfacing.
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on mismatch "
                        "with the focused tab's cwd (ANTS-1372).");
                    // ANTS-1359 — cache-control args.
                    QJsonObject forceProp;
                    forceProp["type"] = "boolean";
                    forceProp["description"] = QStringLiteral(
                        "Bypass the in-process build-cache (ANTS-1359) "
                        "and force a fresh run. Useful after a system "
                        "update or any change to a `.gitignore`d build "
                        "input. Default false.");
                    QJsonObject probeProp;
                    probeProp["type"] = "boolean";
                    probeProp["description"] = QStringLiteral(
                        "Return the cached response if present; else "
                        "return {ok:true, cache_miss:true} without "
                        "running gates. Default false. Mutually "
                        "exclusive with force_refresh. ANTS-1497: "
                        "treated as a read-only call — the ANTS-1372 "
                        "focused-tab cwd gate is bypassed so a session "
                        "in project B can probe its own cache while "
                        "Ants happens to focus a different tab.");
                    QJsonObject props;
                    props["gates"]          = gatesProp;
                    props["max_log_lines"]  = linesProp;
                    props["timeout_sec"]    = timeoutProp;
                    props["caller_cwd"]     = callerProp;
                    props["force_refresh"]  = forceProp;
                    props["cache_only"]     = probeProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1351 — audit_run (server-side audit runner v1).
                {
                    QJsonObject t;
                    t["name"] = "audit_run";
                    t["description"] = QStringLiteral(
                        "Run the project's external audit tools "
                        "(cppcheck/clazy/ruff/bandit/semgrep/gitleaks/"
                        "trivy/shellcheck/mypy) server-side and return "
                        "a structured envelope + SARIF path instead of "
                        "shipping each tool's raw output through parent "
                        "context. v1 ships infrastructure: tools run "
                        "with scrubbed env, absolute-path resolution, "
                        "per-tool wall-clock cap (default 30 s, [5, 60]), "
                        "aggregate cap min(N*cap*1.5, 240 s). Caller "
                        "supplies tools list (auto-detect when empty), "
                        "scope (auto / files / since-tag:X / branch-diff), "
                        "and optional top_findings_count for inline "
                        "result preview. Required: caller_cwd (ANTS-1404 "
                        "Required gate). ANTS-1555: SARIF now lands in "
                        "`<root>/.audit_cache/audit-<iso>-<sha>.sarif` "
                        "(same dir AuditDialog GUI writes to), and the "
                        "envelope carries `cache_path` + `prior_run` "
                        "{iso_timestamp, commit, sarif} for the prior "
                        "sweep — anchor for ANTS-1504 since-last-run "
                        "mode. /tmp fallback when the root is read-only. "
                        "See docs/specs/ANTS-1351.md + ANTS-1555.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to run the static-analysis sweep without "
                        "leaving Ants. Pairs with last_audit_summary "
                        "for the compact read afterwards.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject toolsProp;
                    toolsProp["type"] = "array";
                    toolsProp["description"] = QStringLiteral(
                        "Subset of {cppcheck, clazy, clang-tidy, ruff, "
                        "bandit, semgrep, gitleaks, trivy, shellcheck, "
                        "mypy}. Empty / omitted = auto-detect all "
                        "runnable. clang-tidy is opt-in (no default "
                        "argv beyond `-p build/`); pair it with "
                        "`paths` + `checks` for the scoped sweep "
                        "pattern (ANTS-1512).");
                    QJsonObject scopeProp;
                    scopeProp["type"] = "string";
                    scopeProp["description"] = QStringLiteral(
                        "Audit scope. \"auto\" (default) = full-tree "
                        "minus exclusions, BUT some tools degrade to "
                        "changed-since-fork-point on a clean working "
                        "tree (ANTS-1456 — RetroArch CC session saw "
                        "a fresh no-diff tree return total_raw:0 + "
                        "executionSuccessful:true, which reads as "
                        "\"audit clean\" when the truth is \"nothing "
                        "to audit\"). \"files\" = changed-since-fork-"
                        "point only; \"since-tag:<tag>\" = git diff "
                        "vs <tag> (tag sanitised, INV-15); "
                        "\"branch-diff\" = git diff main..HEAD. For "
                        "a deterministic full sweep, pick "
                        "since-tag:<earliest-tag> rather than "
                        "\"auto\".");
                    QJsonObject capProp;
                    capProp["type"] = "integer";
                    capProp["description"] = QStringLiteral(
                        "Per-tool wall-clock cap in seconds, [5, 60]. "
                        "Default 30. Out-of-range → bad_args.");
                    QJsonObject suppProp;
                    suppProp["type"] = "string";
                    suppProp["description"] = QStringLiteral(
                        "\"auto\" (default) loads .audit_suppress + "
                        ".audit_allowlist.json; \"none\" ignores both; "
                        "\"path:<file>\" loads the named file.");
                    QJsonObject formatsProp;
                    formatsProp["type"] = "array";
                    formatsProp["description"] = QStringLiteral(
                        "Subset of [\"sarif\",\"html\"]. Default "
                        "[\"sarif\"]; HTML opt-in.");
                    QJsonObject topProp;
                    topProp["type"] = "integer";
                    topProp["description"] = QStringLiteral(
                        "When > 0, inline first N findings into envelope "
                        "as top_findings[] (avoids the SARIF-Read round "
                        "trip). [0, 100]; out-of-range → bad_args.");
                    // ANTS-1512 — scoped-check mode.
                    QJsonObject pathsProp;
                    pathsProp["type"] = "array";
                    pathsProp["description"] = QStringLiteral(
                        "Project-relative paths constraining the "
                        "tool's invocation (e.g. [\"menu/drivers/\", "
                        "\"gfx/\"]). Currently honoured by cppcheck "
                        "and clang-tidy as positional args. Each "
                        "entry is sanitised through isAuditArgSafe; "
                        "any failing entry rejects the whole call "
                        "with code:\"bad_args\".");
                    QJsonObject checksProp;
                    checksProp["type"] = "array";
                    checksProp["description"] = QStringLiteral(
                        "Check IDs constraining the tool's enabled "
                        "rule set (e.g. [\"bugprone-integer-division\"]). "
                        "Currently honoured by clang-tidy only "
                        "(rendered as `--checks=-*,<joined>`); "
                        "other tools refuse with code:\"bad_args\" "
                        "rather than silently ignore. Each entry "
                        "must match ^-?[A-Za-z0-9_*.,-]+$ "
                        "(length ≤ 128).");
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on mismatch "
                        "with the focused tab's cwd (ANTS-1372).");
                    QJsonObject props;
                    props["tools"]                = toolsProp;
                    props["scope"]                = scopeProp;
                    props["cap_per_tool_seconds"] = capProp;
                    props["suppressions"]         = suppProp;
                    props["formats"]              = formatsProp;
                    props["top_findings_count"]   = topProp;
                    props["paths"]                = pathsProp;
                    props["checks"]               = checksProp;
                    props["caller_cwd"]           = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1397 — test_audit_partition (v1 trio start)
                {
                    QJsonObject t;
                    t["name"] = "test_audit_partition";
                    t["description"] = QStringLiteral(
                        "Phase 1 of the test_audit trio. Detect "
                        "test framework, walk test files, pack into "
                        "chunks (size 12 default, [4,30]), run "
                        "pre-pass regex scan, return paginated chunks "
                        "+ partition_token. v1 uses a hardcoded "
                        "pre-pass pattern set; v2 ships a project "
                        "JSON resource. dimensions=\"auto\" (default) "
                        "or \"csv:<d1,d2>\". scope=\"auto\"/\"path:<sub>\"/"
                        "\"files:<csv>\". Required: caller_cwd. "
                        "Per-chunk `pre_pass_dimensions[]` (ANTS-1487 "
                        "renamed from dimension_hints; ANTS-1461) "
                        "reflects which audit dimensions the pre-pass "
                        "regex matched on chunk source — it is NOT a "
                        "finding-density predictor. Real finding "
                        "distribution is only known after the "
                        "subagent reviews the chunk. Always seed the "
                        "subagent with the envelope-level "
                        "`dimensions_active[]` (full lane list); "
                        "`pre_pass_dimensions[]` is a prioritisation "
                        "hint, not a coverage allow-list. Envelope "
                        "also carries `pre_pass_chunk_ids[]` "
                        "(ANTS-1489) — the subset of chunk IDs the "
                        "pre-pass found anything on, so callers can "
                        "skip per-chunk brief() calls for empties. "
                        "See docs/specs/ANTS-1397.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to split a test corpus for multi-"
                        "reviewer test-audit dispatch. Phase 1 of "
                        "the trio; pairs with test_audit_brief.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject sP; sP["type"] = "string";
                    QJsonObject dP; dP["type"] = "string";
                    // ANTS-1519 — chunk_size default + bounds surfaced on
                    // the property itself so a session reading the JSON
                    // schema (not the prose description) still knows the
                    // default. Server-side clamp [4, 30] lives in
                    // testauditengine.cpp; default 12 lives in
                    // testauditengine.h PartitionRequest::chunkSize.
                    QJsonObject cP;
                    cP["type"] = "integer";
                    cP["default"] = 12;
                    cP["minimum"] = 4;
                    cP["maximum"] = 30;
                    cP["description"] = QStringLiteral(
                        "Files per chunk. Default 12; clamped to [4, 30] "
                        "server-side. Override via "
                        "<projectPath>/.test-audit/partition.json.");
                    QJsonObject qP;
                    qP["type"] = "boolean";
                    qP["default"] = false;
                    qP["description"] = QStringLiteral(
                        "Quick mode — skip pre-pass regex scan. Default "
                        "false.");
                    QJsonObject oP;
                    oP["type"] = "integer";
                    oP["default"] = 0;
                    oP["minimum"] = 0;
                    oP["description"] = QStringLiteral(
                        "0-based start index into chunks[]. Default 0.");
                    QJsonObject lP;
                    lP["type"] = "integer";
                    lP["default"] = -1;
                    lP["description"] = QStringLiteral(
                        "Cap on chunks[] length. Default -1 (no caller "
                        "limit; server may still auto-truncate large "
                        "envelopes).");
                    QJsonObject ccwd; ccwd["type"] = "string";
                    QJsonObject props;
                    props["scope"] = sP; props["dimensions"] = dP;
                    props["chunk_size"] = cP; props["quick"] = qP;
                    props["offset"] = oP; props["limit"] = lP;
                    props["caller_cwd"] = ccwd;
                    schema["properties"] = props;
                    QJsonArray req; req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1397 — test_audit_brief
                {
                    QJsonObject t;
                    t["name"] = "test_audit_brief";
                    t["description"] = QStringLiteral(
                        "Phase 2 of the test_audit trio. Return "
                        "structured per-chunk manifest "
                        "(source_paths, dimensions, framework_context, "
                        "pre_pass_findings) — NO `brief` string field "
                        "(caller composes the subagent prompt from "
                        "structured siblings). Requires "
                        "partition_token from a prior partition call.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to assemble the brief for one "
                        "test-audit chunk. Phase 2 of the trio; "
                        "run after test_audit_partition.");
                    QJsonObject schema; schema["type"] = "object";
                    QJsonObject cP; cP["type"] = "string";
                    QJsonObject pP; pP["type"] = "string";
                    QJsonObject ccwd; ccwd["type"] = "string";
                    QJsonObject props;
                    props["chunk_id"] = cP;
                    props["partition_token"] = pP;
                    props["caller_cwd"] = ccwd;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("chunk_id");
                    req.append("partition_token");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1397 — test_audit_synthesis_prompt
                // ANTS-1455 — adds allow_outside_project, mode, offset, limit.
                {
                    QJsonObject t;
                    t["name"] = "test_audit_synthesis_prompt";
                    t["description"] = QStringLiteral(
                        "Phase 3 of the test_audit trio. Read per-"
                        "chunk reports from <reports_dir>, fence each "
                        "via <chunk_report file=\"…\"> tags (prompt-"
                        "injection defence, INV-8), and return a "
                        "synth prompt. Three modes: "
                        "mode:\"summary\" (default) returns stats + "
                        "pointers only — top_dimensions, file_index, "
                        "severity_histograms (ANTS-1488), and a chunk "
                        "inventory annotated with per-chunk finding "
                        "counts; ≤ 16 KiB; for actionable text use "
                        "mode:\"full\" + offset/limit, or read per-"
                        "chunk report files directly. "
                        "mode:\"full\" returns the verbatim fenced "
                        "bundle, paginated via offset/limit (default "
                        "limit:5; -1 = all). "
                        "mode:\"hybrid\" (ANTS-1486) returns the "
                        "summary header + the top-N highest-finding-"
                        "count chunks verbatim (N comes from `limit`, "
                        "default 3) — call once, get both navigation "
                        "and actionable text for heavy chunks without "
                        "paging. allow_outside_project:true accepts "
                        "an absolute reports_dir (e.g. /tmp) for "
                        "ephemeral CI workflows (ANTS-1455). Refusals: "
                        "bad_mode, reports_dir_outside_root, "
                        "reports_dir_unreadable, reports_dir_empty.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to draft the synthesis prompt that "
                        "folds N test-audit chunks back into one "
                        "report. Phase 3 of the trio.");
                    QJsonObject schema; schema["type"] = "object";
                    QJsonObject pP; pP["type"] = "string";
                    QJsonObject rP; rP["type"] = "string";
                    QJsonObject aP; aP["type"] = "object";
                    QJsonObject ccwd; ccwd["type"] = "string";
                    QJsonObject aopP; aopP["type"] = "boolean";
                    aopP["description"] = QStringLiteral(
                        "ANTS-1455 — when true, reports_dir may resolve "
                        "outside callerCwd (still NFC + control-char "
                        "checked + canonicalised). Default false.");
                    QJsonObject mP; mP["type"] = "string";
                    mP["enum"] = QJsonArray{QStringLiteral("summary"),
                                            QStringLiteral("full"),
                                            QStringLiteral("hybrid")};
                    mP["description"] = QStringLiteral(
                        "ANTS-1455 — \"summary\" (default) returns "
                        "counts + top pointers + severity histograms; "
                        "\"full\" returns the verbatim fenced bundle "
                        "(paginated); \"hybrid\" (ANTS-1486) returns "
                        "summary header + top-N highest-finding-count "
                        "chunks verbatim.");
                    QJsonObject oP; oP["type"] = "integer";
                    oP["description"] = QStringLiteral(
                        "Chunk offset for mode:\"full\" pagination. "
                        "Default 0. Ignored in summary/hybrid.");
                    QJsonObject lP; lP["type"] = "integer";
                    lP["description"] = QStringLiteral(
                        "Chunk limit for mode:\"full\" (default 5; "
                        "-1 returns all). For mode:\"hybrid\", this "
                        "is N — the number of top chunks to inline "
                        "verbatim (default 3). Ignored in summary.");
                    QJsonObject props;
                    props["partition_token"]       = pP;
                    props["reports_dir"]           = rP;
                    props["calibration_anchor"]    = aP;
                    props["caller_cwd"]            = ccwd;
                    props["allow_outside_project"] = aopP;
                    props["mode"]                  = mP;
                    props["offset"]                = oP;
                    props["limit"]                 = lP;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("partition_token");
                    req.append("reports_dir");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1397 — test_audit_fold_in
                {
                    QJsonObject t;
                    t["name"] = "test_audit_fold_in";
                    t["description"] = QStringLiteral(
                        "Phase 4 of the test_audit trio. Render "
                        "actionable findings as ROADMAP bullets via "
                        "RoadmapFoldIn::allocateIds + insertBlock "
                        "(engine-level delegation, NOT MCP re-entry "
                        "— INV-3). Single batched write: all N IDs "
                        "allocated upfront, one insertBlock call. "
                        "Per-project state: <caller_cwd>/.roadmap-"
                        "counter (advisory-locked via flock; ANTS-1490 "
                        "falls back to .roadmap-counter.lock O_EXCL "
                        "rename-lock on filesystems where flock returns "
                        "systemic errors). On id_counter_failed the "
                        "error message names the counter path so the "
                        "caller can clear a stale .lock sibling.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to merge a finished test-audit set "
                        "back into ROADMAP.md as a fold-in block. "
                        "Phase 4; mutates ROADMAP — caller_cwd "
                        "required.");
                    QJsonObject schema; schema["type"] = "object";
                    QJsonObject aP; aP["type"] = "array";
                    QJsonObject fP; fP["type"] = "string";
                    QJsonObject fsP; fsP["type"] = "integer";
                    QJsonObject dP; dP["type"] = "array";
                    QJsonObject rfP; rfP["type"] = "integer";
                    QJsonObject ccwd; ccwd["type"] = "string";
                    QJsonObject props;
                    props["actionable"]    = aP;
                    props["framework"]     = fP;
                    props["files_scanned"] = fsP;
                    props["dimensions"]    = dP;
                    props["raw_findings"]  = rfP;
                    props["caller_cwd"]    = ccwd;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("actionable");
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
                    t["selection_hint"] = QStringLiteral(
                        "Use when starting any non-trivial "
                        "implementation task to scaffold spec.md "
                        "+ test wiring + CHANGELOG entry skeleton.");
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
                        "total_wrap_bytes, tools_called}; per-tool "
                        "entry adds {wrap_bytes, duration_us_min/"
                        "max/mean}. Sorted by est_tokens_saved "
                        "descending. Pure read by default; pass "
                        "reset:true to read-and-clear in one "
                        "round-trip. No required args.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to inspect this session's MCP-call "
                        "cost. Control-plane (no caller_cwd); "
                        "read-only by default.");
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
                // ANTS-1360 — mcp_trace: ring-buffer slice of last
                // tools/call dispatches. Privacy-first: shape +
                // lengths + hashes only, no raw arg values.
                {
                    QJsonObject t;
                    t["name"] = "mcp_trace";
                    t["description"] = QStringLiteral(
                        "Return the last N tool/call records observed "
                        "by this Ants Terminal MCP server. Useful for "
                        "debugging third-party MCP integrations. "
                        "Records are structured (shape + lengths + "
                        "hashes only) — raw argument values are never "
                        "stored. ANTS-1360.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when debugging MCP tool behaviour. "
                        "Returns recent dispatcher trace events; "
                        "read-only ring-buffer.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject sinceProp;
                    sinceProp["type"] = "integer";
                    sinceProp["minimum"] = 0;
                    sinceProp["description"] = QStringLiteral(
                        "Return only records with id >= since "
                        "(inclusive). Use 0 to start from the oldest "
                        "record currently in the ring.");
                    QJsonObject limitProp;
                    limitProp["type"] = "integer";
                    limitProp["minimum"] = 1;
                    limitProp["maximum"] = 200;
                    limitProp["default"] = 50;
                    limitProp["description"] = QStringLiteral(
                        "Maximum records returned. Hard cap = ring "
                        "cap (200). Out-of-range values are clamped.");
                    QJsonObject props;
                    props["since"] = sinceProp;
                    props["limit"] = limitProp;
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
                        "plugins / per-spec). Spec lanes (ANTS-1411): "
                        "every `*.md` under `docs/specs/` is a "
                        "candidate; `ANTS-NNNN.md` files are gated on "
                        "ROADMAP active set (legacy filter), other "
                        "filename shapes (e.g. `DS01.md`, `P04.md`) "
                        "are unconditionally surfaced. Spec lanes "
                        "capped at 12 (most-recently-modified). "
                        "Override (ANTS-1412): "
                        "`<projectPath>/.cold-eyes/partition.json` with "
                        "shape `{\"version\":1,\"lanes\":[{\"name\":"
                        "\"<id>\",\"summary\":\"<text>\",\"doc_paths\":"
                        "[\"<project-rel>\",...]}]}`. Each doc_path "
                        "must resolve inside the project root; "
                        "absolute paths and symlink escapes are "
                        "dropped per INV-13. Malformed overrides "
                        "fall back to the default partition and "
                        "surface the cause in `override_warning`. "
                        "Optional: scope (\"default\" / \"docs_only\" "
                        "/ \"contracts_only\").");
                    t["selection_hint"] = QStringLiteral(
                        "Use to split docs/specs for multi-reviewer "
                        "cold-eyes dispatch. Pairs with "
                        "cold_eyes_brief.");
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
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
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
                        "Required: lane (string). Optional (ANTS-1508): "
                        "doc_paths[] — when `lane` is not in the "
                        "auto-partition, the brief is synthesised "
                        "from these caller-supplied paths instead. "
                        "Paths must be project-relative and resolve "
                        "inside the project root (INV-13 enforced); "
                        "absolute paths and symlink escapes are "
                        "rejected silently.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to assemble the brief for one "
                        "cold-eyes chunk. Run after "
                        "cold_eyes_partition. Pass doc_paths[] for "
                        "ad-hoc lanes the auto-partition didn't "
                        "surface.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject laneProp;
                    laneProp["type"] = "string";
                    laneProp["description"] = QStringLiteral(
                        "Lane name. May be a partition-derived name "
                        "(see cold_eyes_partition) or an ad-hoc label "
                        "paired with `doc_paths[]`.");
                    QJsonObject docPathsProp;
                    docPathsProp["type"] = "array";
                    QJsonObject docPathsItems;
                    docPathsItems["type"] = "string";
                    docPathsProp["items"] = docPathsItems;
                    docPathsProp["description"] = QStringLiteral(
                        "Optional. Project-relative doc paths for the "
                        "lane. Used only when `lane` is absent from "
                        "the auto-partition. Each entry is anchored "
                        "inside the project root.");
                    QJsonObject props;
                    props["lane"]      = laneProp;
                    props["doc_paths"] = docPathsProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
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
                        "Cross-doc corroboration filter. Input: "
                        "EITHER `reports` (inline map of "
                        "{lane: report_text}, ANTS-1509) OR "
                        "`reports_dir` (project-relative directory of "
                        "*.md files, ANTS-1282 — saves parent context "
                        "by reading from disk server-side). Returns "
                        "findings cited by >= min_lanes distinct "
                        "reports at the same (file, line). Pure regex "
                        "pass; no LLM. Mirrors indie_review_corroborate. "
                        "Provide exactly one of `reports` or "
                        "`reports_dir`. Optional: min_lanes (default 2).");
                    t["selection_hint"] = QStringLiteral(
                        "Use to surface drift across two related "
                        "docs (spec vs CHANGELOG, ROADMAP vs spec). "
                        "Regex pass; no LLM.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject reportsProp;
                    reportsProp["type"] = "object";
                    reportsProp["description"] = QStringLiteral(
                        "Map of {lane_name: report_markdown}. "
                        "Mutually exclusive with reports_dir.");
                    QJsonObject rdProp;
                    rdProp["type"] = "string";
                    rdProp["description"] = QStringLiteral(
                        "Project-relative path to a directory of "
                        "*.md report files. Lane name = filename "
                        "stem. Top level only; sub-dirs not "
                        "recursed. Mutually exclusive with reports.");
                    QJsonObject mlProp;
                    mlProp["type"]    = "integer";
                    mlProp["default"] = 2;
                    mlProp["minimum"] = 1;
                    mlProp["description"] = QStringLiteral(
                        "Minimum distinct lanes citing a (file, "
                        "line) for it to count as corroborated.");
                    QJsonObject props;
                    props["reports"]     = reportsProp;
                    props["reports_dir"] = rdProp;
                    props["min_lanes"]   = mlProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    // INV-1 XOR is enforced at the handler layer
                    // (cmdColdEyesCrossDocDiff), not the schema —
                    // JSON Schema's oneOf is verbose and Claude
                    // Code's schema validator handles oneOf poorly
                    // (same rationale as indie_review_corroborate
                    // post-ANTS-1282).
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
                        "findings. Default (`id_allocation:\"auto\"`) "
                        "allocates IDs from .roadmap-counter (via "
                        "RoadmapFoldIn::allocateIds) and renders "
                        "bullets with `[ANTS-NNNN]` prefixes — "
                        "**REQUIRES** the project to follow "
                        "docs/standards/roadmap-format.md (counter "
                        "file + ID-prefixed bullets). "
                        "`id_allocation:\"skip\"` skips the counter "
                        "touch and emits prefix-free bullets — use "
                        "for projects whose roadmap uses ad-hoc "
                        "headings (e.g. \"Pass N.M\"). When a "
                        "release-block heading is found via "
                        "findActiveReleaseHeading (or supplied via "
                        "`release_block_heading`), the block is "
                        "atomically inserted into ROADMAP.md; "
                        "otherwise the block is returned in "
                        "`block` for caller-side splicing. "
                        "Required: actionable (array), caller_cwd "
                        "(string — your $PWD; ANTS-1372).");
                    t["selection_hint"] = QStringLiteral(
                        "Use to fold a cold-eyes set into "
                        "ROADMAP.md. id_allocation:\"skip\" for "
                        "projects without .roadmap-counter / "
                        "[PROJ-NNNN] IDs. caller_cwd required.");
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
                        "RoadmapFoldIn::findActiveReleaseHeading. "
                        "Required when the project's ROADMAP.md "
                        "has no `## (target: YYYY-NN)` or "
                        "`## N.M.P — …` line (e.g. freeform "
                        "headings like \"Pass N.M\").");
                    // ANTS-1510 — id_allocation freeform-mode switch.
                    QJsonObject iaProp;
                    iaProp["type"] = "string";
                    QJsonArray iaEnum;
                    iaEnum.append("auto");
                    iaEnum.append("skip");
                    iaProp["enum"] = iaEnum;
                    iaProp["default"] = "auto";
                    iaProp["description"] = QStringLiteral(
                        "ID allocation mode. \"auto\" (default) "
                        "pulls N consecutive IDs from "
                        ".roadmap-counter and renders bullets "
                        "prefixed with `[ANTS-NNNN]` per "
                        "docs/standards/roadmap-format.md § 3.5.1. "
                        "\"skip\" suppresses the counter touch and "
                        "emits prefix-free bullets — use for "
                        "projects whose roadmap doesn't follow the "
                        "shareable ID scheme (e.g. RetroDB's \"Pass "
                        "N.M\" headings). Echoed in the response "
                        "envelope.");
                    // ANTS-1389 — caller_cwd schema surfacing.
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Mutating verbs refuse on "
                        "mismatch with the focused tab's cwd "
                        "(ANTS-1372).");
                    QJsonObject props;
                    props["actionable"]            = aProp;
                    props["date_iso"]              = dProp;
                    props["release_block_heading"] = hProp;
                    props["id_allocation"]         = iaProp;
                    props["caller_cwd"]            = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("actionable");
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1413 — cold_eyes_single_doc. Cross-consistency
                // brief for one doc without the full multi-lane
                // partition + brief workflow. Returns the doc's
                // related-doc neighbourhood (same-dir siblings,
                // project standards, root contracts).
                {
                    QJsonObject t;
                    t["name"] = "cold_eyes_single_doc";
                    t["description"] = QStringLiteral(
                        "Single-doc cross-consistency brief. Given a "
                        "`doc_path`, return the docs it should stay "
                        "consistent with: same-dir siblings, project "
                        "standards, root contracts. Useful for "
                        "sanity-checking a freshly drafted spec "
                        "without committing a "
                        ".cold-eyes/partition.json or dispatching the "
                        "full multi-lane sweep. Returns {doc_path, "
                        "summary, related:{same_dir_siblings, "
                        "standards, root_contracts}, "
                        "recommended_reviewers}.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when reviewing one new spec — gives "
                        "the cross-consistency neighbourhood without "
                        "running cold_eyes_partition + brief.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject dpProp;
                    dpProp["type"] = "string";
                    dpProp["description"] = QStringLiteral(
                        "Project-relative doc path. Anchored under "
                        "project root (INV-13).");
                    QJsonObject props;
                    props["doc_path"]   = dpProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("doc_path");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1414 — cross_doc_diff. Lane-source-agnostic
                // alias for cold_eyes_cross_doc_diff / indie_review_-
                // corroborate's regex hotspot primitive. Same args,
                // same envelope shape — different name so callers
                // working from arbitrary reviewer-report bundles
                // don't have to pick a cold-eyes or indie-review
                // framing.
                {
                    QJsonObject t;
                    t["name"] = "cross_doc_diff";
                    t["description"] = QStringLiteral(
                        "Regex hotspot corroboration across reviewer "
                        "reports. Input: EITHER `reports` (inline map "
                        "of {lane: report_text}) OR `reports_dir` "
                        "(project-relative directory of *.md files). "
                        "Returns findings cited by >= min_lanes "
                        "distinct reports at the same (file, line). "
                        "Pure regex pass; no LLM. Lane-source-"
                        "agnostic alias for "
                        "cold_eyes_cross_doc_diff / "
                        "indie_review_corroborate (same engine "
                        "primitive). Provide exactly one of `reports` "
                        "or `reports_dir`. Optional: min_lanes "
                        "(default 2).");
                    t["selection_hint"] = QStringLiteral(
                        "Use when corroborating an arbitrary "
                        "reviewer-report bundle without committing "
                        "to the cold-eyes vs indie-review framing.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject reportsProp;
                    reportsProp["type"] = "object";
                    reportsProp["description"] = QStringLiteral(
                        "Map of {lane_name: report_markdown}. "
                        "Mutually exclusive with reports_dir.");
                    QJsonObject rdProp;
                    rdProp["type"] = "string";
                    rdProp["description"] = QStringLiteral(
                        "Project-relative path to a directory of "
                        "*.md report files. Lane name = filename "
                        "stem. Top level only; sub-dirs not "
                        "recursed. Mutually exclusive with reports.");
                    QJsonObject mlProp;
                    mlProp["type"]    = "integer";
                    mlProp["default"] = 2;
                    mlProp["minimum"] = 1;
                    mlProp["description"] = QStringLiteral(
                        "Minimum distinct lanes citing a (file, "
                        "line) for it to count as corroborated.");
                    QJsonObject props;
                    props["reports"]     = reportsProp;
                    props["reports_dir"] = rdProp;
                    props["min_lanes"]   = mlProp;
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    // XOR enforced at the handler (cmdCrossDocDiff),
                    // not the schema — mirrors ANTS-1509 rationale.
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1400 — caller_cwd_info diagnostic verb.
                // Surfaces the ResolvedRoot::Source enum so callers
                // can dispatch on which branch fired without
                // invoking a tab-anchored tool first.
                {
                    QJsonObject t;
                    t["name"] = "caller_cwd_info";
                    t["description"] = QStringLiteral(
                        "Diagnose how Ants would resolve your "
                        "caller_cwd. Returns "
                        "{ok:true, source:'ExplicitMatch'|"
                        "'EmptyFallback'|'NoMatch'|'Unresolvable', "
                        "resolved_cwd:'...', tab_index:N?}. No side "
                        "effects — does not read scrollback, run "
                        "git, or write any state. Pass caller_cwd "
                        "to test resolution; omit it to see the "
                        "empty-fallback behaviour (focused tab). "
                        // ANTS-1578 — discoverability hint mirrored
                        // into the description so callers that only
                        // see the description text (not the separate
                        // selection_hint field) still find the verb
                        // when chasing no_roadmap_loaded / cwd_mismatch.
                        "Use this FIRST when a project-scoped read "
                        "returns `no_roadmap_loaded` or any tool "
                        "returns `cwd_mismatch` — confirms which "
                        "project's data the tool would have been "
                        "operating on. "
                        "See docs/specs/ANTS-1400.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use FIRST when a read tool returns "
                        "no_roadmap_loaded, cwd_mismatch, or any "
                        "*-not-found refusal — confirms which "
                        "project's data the tool would have read. "
                        "No side effects.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD or an arbitrary path you want "
                        "to test. Empty/absent → EmptyFallback "
                        "(returns focused-tab info).");
                    QJsonObject props;
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    schema["required"] = QJsonArray();
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1424 — roadmap_log: append a new bullet to
                // ROADMAP.md. Mutates project state (counter +
                // markdown), so Required-contract gated and path-
                // validated. Schema mirrors the field set in
                // docs/standards/roadmap-format.md § 3.5.
                {
                    QJsonObject t;
                    t["name"] = "roadmap_log";
                    t["description"] = QStringLiteral(
                        "Append a new bullet to ROADMAP.md, or flip "
                        "the status of an existing bullet on either "
                        "a GFM-task-list or Ants-v1 emoji-status "
                        "roadmap (auto-detected; ANTS-1441). Mode "
                        "picked by `op` (default \"append\"). "
                        "op:\"append\" (ANTS-1424) — allocates the "
                        "next stable ID from .roadmap-counter, "
                        "formats the bullet per the project's "
                        "roadmap-format spec, and inserts at the end "
                        "of `section` (slug from roadmap_query). "
                        "Required: caller_cwd, section, status "
                        "(planned/in-progress/shipped/considered), "
                        "headline, kind, source. Optional: body, "
                        "layman, lanes[], id_hint. "
                        "op:\"flip\" (ANTS-1428) — flips a bullet's "
                        "status without touching anything else; "
                        "injects an Obsidian-style `^prefix-NNNN` "
                        "anchor on first touch as the durable "
                        "handle. Required: caller_cwd, to_status, "
                        "and one of (id | anchor | headline). "
                        "Optional: prefix_hint. Refusal codes: "
                        "bullet_not_found, bullet_ambiguous, "
                        "anchor_unsafe_context, bad_op_combo, "
                        "unrecognised_format. Returns {ok, id?, "
                        "file, line, bytes_written, ...op-specific} "
                        "or {ok:false, error, code}.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to append a new bullet or flip an "
                        "existing one's status on ROADMAP.md. "
                        "Mutates project state — caller_cwd "
                        "required.");
                    QJsonObject schema;
                    schema["type"] = "object";

                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. REQUIRED — anchors the write to "
                        "your project's ROADMAP.md.");

                    QJsonObject sectionProp;
                    sectionProp["type"] = "string";
                    sectionProp["description"] = QStringLiteral(
                        "Slug of a ## or ### heading "
                        "(e.g. \"performance-2\"). Get valid slugs "
                        "from `roadmap_query`'s section echo. New "
                        "bullet appends at the end of this section.");

                    QJsonObject statusProp;
                    statusProp["type"] = "string";
                    QJsonArray statusEnum;
                    statusEnum.append("planned");
                    statusEnum.append("in-progress");
                    statusEnum.append("shipped");
                    statusEnum.append("considered");
                    statusProp["enum"] = statusEnum;
                    statusProp["description"] = QStringLiteral(
                        "Lifecycle status. Mapped to 📋/🚧/✅/💭 "
                        "emoji by the verb.");

                    QJsonObject headlineProp;
                    headlineProp["type"]      = "string";
                    headlineProp["maxLength"] = 200;
                    headlineProp["description"] = QStringLiteral(
                        "One-line bold headline ending in a period "
                        "(per roadmap-format.md § 3.5).");

                    QJsonObject kindProp;
                    kindProp["type"] = "string";
                    QJsonArray kindEnum;
                    kindEnum.append("implement");
                    kindEnum.append("fix");
                    kindEnum.append("audit-fix");
                    kindEnum.append("review-fix");
                    kindEnum.append("doc");
                    kindEnum.append("doc-fix");
                    kindEnum.append("refactor");
                    kindEnum.append("test");
                    kindEnum.append("chore");
                    kindEnum.append("release");
                    kindEnum.append("perf");
                    kindEnum.append("security");
                    kindEnum.append("feature");
                    kindEnum.append("enhancement");
                    kindEnum.append("investigate");
                    kindEnum.append("research");
                    kindEnum.append("accessibility");
                    kindEnum.append("optimize");
                    kindEnum.append("package");
                    kindEnum.append("marketing");
                    kindEnum.append("ux");
                    kindProp["enum"] = kindEnum;
                    kindProp["description"] = QStringLiteral(
                        "Work category — see roadmap-format.md § 3.5.3.");

                    QJsonObject sourceProp;
                    sourceProp["type"]      = "string";
                    sourceProp["maxLength"] = 200;
                    sourceProp["description"] = QStringLiteral(
                        "Provenance (e.g. \"user-request-2026-05-16\", "
                        "\"in-session-2026-05-16\", "
                        "\"indie-review-2026-05-14 lane-2 M3\").");

                    QJsonObject bodyProp;
                    bodyProp["type"]      = "string";
                    bodyProp["maxLength"] = 4000;
                    bodyProp["description"] = QStringLiteral(
                        "Optional body prose. Written verbatim with "
                        "a 2-space indent; pre-wrap to ~70 columns.");

                    QJsonObject laymanProp;
                    laymanProp["type"]      = "string";
                    laymanProp["maxLength"] = 400;
                    laymanProp["description"] = QStringLiteral(
                        "Optional one-sentence summary for non-"
                        "technical readers. Renders on the card face "
                        "in the Roadmap dialog (ANTS-1154).");

                    QJsonObject lanesProp;
                    lanesProp["type"] = "array";
                    QJsonObject laneItem;
                    laneItem["type"] = "string";
                    lanesProp["items"] = laneItem;
                    lanesProp["description"] = QStringLiteral(
                        "Optional subsystem owners. Emitted as a "
                        "`Lanes: a, b, c` line.");

                    QJsonObject idHintProp;
                    idHintProp["type"]    = "integer";
                    idHintProp["minimum"] = 1;
                    idHintProp["description"] = QStringLiteral(
                        "Optional explicit ID under op:\"append\". "
                        "Must exceed the current .roadmap-counter "
                        "value or returns code=id_taken. Not accepted "
                        "under op:\"flip\" (bad_op_combo).");

                    // ANTS-1428 flip-mode fields.
                    QJsonObject opProp;
                    opProp["type"] = "string";
                    QJsonArray opEnum;
                    opEnum.append("append");
                    opEnum.append("flip");
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral(
                        "Verb mode. Default \"append\" (ANTS-1424). "
                        "\"flip\" routes to the status-flip path "
                        "(ANTS-1428; works on GFM-task-list and "
                        "Ants-v1 emoji formats — ANTS-1441).");
                    QJsonObject toStatusProp;
                    toStatusProp["type"] = "string";
                    QJsonArray toStatusEnum;
                    toStatusEnum.append("planned");
                    toStatusEnum.append("in-progress");
                    toStatusEnum.append("shipped");
                    toStatusEnum.append("considered");
                    toStatusProp["enum"] = toStatusEnum;
                    toStatusProp["description"] = QStringLiteral(
                        "Target lifecycle status for op:\"flip\". "
                        "Mapped to 📋/🚧/✅/💭 emoji by the verb.");
                    QJsonObject idProp;
                    idProp["type"] = "string";
                    idProp["description"] = QStringLiteral(
                        "Bold-ID locator for op:\"flip\" (e.g. "
                        "\"Sh4\", \"VEST-0042\"). Highest precedence "
                        "locator; wins over anchor when both are "
                        "passed (INV-12).");
                    QJsonObject anchorProp;
                    anchorProp["type"] = "string";
                    anchorProp["description"] = QStringLiteral(
                        "Caret-anchor locator for op:\"flip\" (e.g. "
                        "\"vest-0042\"). Used when no bold-ID is "
                        "available.");
                    QJsonObject prefixHintProp;
                    prefixHintProp["type"] = "string";
                    prefixHintProp["pattern"] = "^[A-Z][A-Z0-9_-]{0,15}$";
                    prefixHintProp["description"] = QStringLiteral(
                        "Optional prefix for newly-injected caret "
                        "anchors under op:\"flip\". Defaults to the "
                        "uppercase first 4 chars of caller_cwd's "
                        "leaf directory.");

                    QJsonObject props;
                    props["caller_cwd"]  = callerProp;
                    props["op"]          = opProp;
                    props["section"]     = sectionProp;
                    props["status"]      = statusProp;
                    props["to_status"]   = toStatusProp;
                    props["headline"]    = headlineProp;
                    props["kind"]        = kindProp;
                    props["source"]      = sourceProp;
                    props["body"]        = bodyProp;
                    props["layman"]      = laymanProp;
                    props["lanes"]       = lanesProp;
                    props["id_hint"]     = idHintProp;
                    props["id"]          = idProp;
                    props["anchor"]      = anchorProp;
                    props["prefix_hint"] = prefixHintProp;
                    schema["properties"] = props;

                    // ANTS-1428 — only caller_cwd is unconditionally
                    // required across both ops. op:"append" needs
                    // section+status+headline+kind+source; op:"flip"
                    // needs to_status + one locator. JSON Schema's
                    // top-level `required` can't express this without
                    // `oneOf`/`allOf`; the verb enforces the
                    // conditional logic at runtime via
                    // missing_field / bad_op_combo refusals.
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1399-INV-1 — tool_info(name) descriptor.
                // Cheaper than re-fetching tools/list when the assistant
                // only needs to refresh memory on one tool's schema.
                // Self-registers so tool_info({name:"tool_info"})
                // round-trips like every other listed tool.
                {
                    QJsonObject t;
                    t["name"] = "tool_info";
                    t["description"] = QStringLiteral(
                        "Fetch a single MCP tool's descriptor — name, "
                        "description, inputSchema. Cheaper than a full "
                        "tools/list refresh (~50–200 B vs ~5 KiB). "
                        "Unknown name returns code=unknown_tool plus "
                        "an `available` list of registered tool names. "
                        "Empty/missing name returns code=missing_name. "
                        "Cache cold (no prior tools/list) returns "
                        "code=tools_not_ready. See "
                        "docs/specs/ANTS-1399.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to fetch one tool's descriptor without "
                        "re-paying for the full tools/list snapshot "
                        "(~80 B vs ~5 KiB). Surfaces selection_hint "
                        "field (ANTS-1453).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject nameProp;
                    nameProp["type"] = "string";
                    nameProp["description"] = QStringLiteral(
                        "Registered tool name (e.g. "
                        "\"verify_changes\", \"file_outline\").");
                    QJsonObject props;
                    props["name"] = nameProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("name"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1430 — project_layout. Pre-cached project file
                // layout (ROADMAP/CHANGELOG/specs/standards/decisions/
                // appstream/counter). Read verb; scans the project tree
                // when the cache is missing, expired (7-day TTL), or
                // any probed path's mtime has advanced. Persists via
                // session_memory under the well-known key
                // `project_layout`. See docs/specs/ANTS-1430.md.
                {
                    QJsonObject t;
                    t["name"] = "project_layout";
                    t["description"] = QStringLiteral(
                        "Per-project file-layout cache. Returns "
                        "{roadmap:{path,format,bullet_count_estimate,"
                        "size_bytes,mtime_ms},changelog:{path,"
                        "size_bytes,mtime_ms},specs_dir,standards_dir,"
                        "adr_dir,appstream_metainfo,counter_file,"
                        "cached:bool}. First call scans (cached:false), "
                        "subsequent within 7-day TTL return the cache "
                        "(cached:true). mtime change on any probed path "
                        "invalidates. Pass force_rescan:true to bypass. "
                        "caller_cwd required (Required contract — "
                        "tenant-hashed storage). ANTS-1435: reads "
                        "anchor to your caller_cwd directly — no "
                        "focused-tab match needed (cross-tab queries "
                        "work). ANTS-1493: probe set widened to also "
                        "cover docs/private/, docs/internal/, "
                        "docs/fork/ (fork-only doc trees) and "
                        "*.metainfo.xml at repo root + pkg/ + data/ + "
                        "share/applications/ — first hit per field "
                        "wins.");
                    t["selection_hint"] = QStringLiteral(
                        "Use as first call when you need to locate "
                        "ROADMAP/CHANGELOG/specs/standards in an "
                        "unfamiliar project layout. Cached.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject forceProp;
                    forceProp["type"] = "boolean";
                    forceProp["description"] = QStringLiteral(
                        "If true, bypasses TTL+mtime check and "
                        "re-scans unconditionally. Default false.");
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. REQUIRED — Required contract "
                        "refuses with caller_cwd_required when empty.");
                    QJsonObject props;
                    props["force_rescan"] = forceProp;
                    props["caller_cwd"]   = callerProp;
                    props["etag_match"]   = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"]  = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
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
                        "op (\"get\"/\"set\"/\"delete\"/\"list\"), "
                        "caller_cwd (your $PWD). ANTS-1435: read ops "
                        "(get, list) anchor to your caller_cwd "
                        "directly — cross-tab reads work. Write ops "
                        "(set, delete) require caller_cwd to match "
                        "the focused Ants tab's cwd (prevents "
                        "confused-deputy writes). key required for "
                        "get/set/delete; value required for set. See "
                        "docs/specs/ANTS-1283.md + "
                        "docs/specs/ANTS-1336.md + "
                        "docs/specs/ANTS-1435.md.");
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
                    // ANTS-1336 — `cwd` is deprecated for one release
                    // (0.7.92). Handler ignores it; schema keeps the
                    // field documented so migration errors point at
                    // the replacement. Field is dropped from the
                    // schema entirely in 0.7.93.
                    QJsonObject cwdProp;
                    cwdProp["type"] = "string";
                    cwdProp["description"] = QStringLiteral(
                        "DEPRECATED (ANTS-1336). The handler ignores "
                        "this field. Pass caller_cwd instead. Removed "
                        "in 0.7.93.");
                    // ANTS-1389 — surface the caller_cwd gate.
                    // ANTS-1336 promoted caller_cwd to required for
                    // every op (previously required only on
                    // set/delete; the read-side bypass was the
                    // tenancy leak).
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. REQUIRED for every op "
                        "(ANTS-1336 — cross-project tenancy gate; "
                        "refuses on mismatch with the focused tab's "
                        "cwd). Previously optional for get/list "
                        "(ANTS-1372 INV-7); now required for those "
                        "too — see docs/specs/ANTS-1336.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to persist tiny per-project notes "
                        "across MCP calls (KB-grain). Self-scoped "
                        "per project; not for large data.");
                    QJsonObject props;
                    props["op"]         = opProp;
                    props["key"]        = keyProp;
                    props["value"]      = valProp;
                    props["cwd"]        = cwdProp;
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("op"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

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
                    // Buckets: (typical, worst). Keep the table flat
                    // so the audit pass can spot drift between tool
                    // additions and budget claims.
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
                        {QStringLiteral("get_git_status"),    {400,  1500}},
                        {QStringLiteral("get_environment"),   {300,  800}},
                        {QStringLiteral("get_scrollback"),    {1500, 8000}},
                        {QStringLiteral("get_text"),          {1200, 6000}},
                        // Repo / docs.
                        {QStringLiteral("roadmap_query"),     {1700, 12000}},
                        {QStringLiteral("roadmap_log"),       {200,  600}},
                        {QStringLiteral("project_layout"),    {600,  2000}},
                        {QStringLiteral("session_memory"),    {200,  1000}},
                        {QStringLiteral("workspace_search"),  {1500, 10000}},
                        {QStringLiteral("file_outline"),      {800,  4000}},
                        {QStringLiteral("git_state"),         {700,  4000}},
                        {QStringLiteral("subsystem"),         {400,  1500}},
                        // Plan + verify.
                        {QStringLiteral("plan_template"),     {1500, 6000}},
                        {QStringLiteral("verify_changes"),    {600,  3000}},
                        // Audit suite.
                        {QStringLiteral("audit_run"),         {4000, 25000}},
                        {QStringLiteral("last_audit_summary"),{600,  2500}},
                        // Debt-sweep suite.
                        {QStringLiteral("debt_sweep_scan"),         {2000, 12000}},
                        {QStringLiteral("debt_sweep_triage_prompt"),{1500, 8000}},
                        {QStringLiteral("debt_sweep_apply_fix"),    {400,  1500}},
                        {QStringLiteral("debt_sweep_defer"),        {200,  800}},
                        // Cold-eyes.
                        {QStringLiteral("cold_eyes_partition"),    {1000, 4000}},
                        {QStringLiteral("cold_eyes_brief"),        {1500, 6000}},
                        {QStringLiteral("cold_eyes_cross_doc_diff"),{1200, 5000}},
                        {QStringLiteral("cold_eyes_fold_in"),      {800,  3000}},
                        {QStringLiteral("cold_eyes_single_doc"),   {800,  3000}},
                        {QStringLiteral("cross_doc_diff"),         {1200, 5000}},
                        // Indie-review.
                        {QStringLiteral("indie_review_partition"),    {1500, 6000}},
                        {QStringLiteral("indie_review_brief"),        {2000, 8000}},
                        {QStringLiteral("indie_review_corroborate"),  {1500, 6000}},
                        {QStringLiteral("indie_review_synthesis_prompt"), {2500, 10000}},
                        {QStringLiteral("indie_review_dispatch"),     {3000, 12000}},
                        {QStringLiteral("indie_review_fold_in"),      {1000, 4000}},
                        // Test-audit.
                        {QStringLiteral("test_audit_partition"),       {1500, 6000}},
                        {QStringLiteral("test_audit_brief"),           {2000, 8000}},
                        {QStringLiteral("test_audit_synthesis_prompt"),{2500, 10000}},
                        {QStringLiteral("test_audit_fold_in"),         {1000, 4000}},
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
                // ANTS-1567 — Music_Production + MAME Curator
                // 2026-05-18 asked for grep-friendlier category
                // labels. Two rename: `memory` → `mcp-state` (the
                // bucket is server-side per-cwd KV state, not
                // terminal/RAM "memory"); `caller_cwd_info` moves
                // from `terminal` to `meta` (it's a diagnostic
                // verb, not a pty-state read). Net effect: every
                // tool still has a non-"other" prefix and the
                // labels match the surface families a session
                // looking at tools/list would expect to grep for.
                auto kindForName = [](const QString &name) -> QString {
                    if (name.startsWith(QStringLiteral("cold_eyes_")))
                        return QStringLiteral("cold-eyes");
                    if (name.startsWith(QStringLiteral("indie_review_")))
                        return QStringLiteral("indie-review");
                    if (name.startsWith(QStringLiteral("test_audit_")))
                        return QStringLiteral("test-audit");
                    if (name.startsWith(QStringLiteral("debt_sweep_")))
                        return QStringLiteral("debt-sweep");
                    if (name.startsWith(QStringLiteral("roadmap_")))
                        return QStringLiteral("roadmap");
                    // ANTS-1414 — lane-source-agnostic alias bucket.
                    if (name == QLatin1String("cross_doc_diff"))
                        return QStringLiteral("cold-eyes");
                    if (name == QLatin1String("audit_run") ||
                        name == QLatin1String("last_audit_summary"))
                        return QStringLiteral("audit");
                    if (name == QLatin1String("verify_changes"))
                        return QStringLiteral("verify");
                    if (name == QLatin1String("git_state"))
                        return QStringLiteral("git");
                    if (name == QLatin1String("workspace_search") ||
                        name == QLatin1String("file_outline") ||
                        name == QLatin1String("project_layout") ||
                        name == QLatin1String("subsystem"))
                        return QStringLiteral("workspace");
                    if (name == QLatin1String("session_memory"))
                        return QStringLiteral("mcp-state");
                    if (name == QLatin1String("plan_template"))
                        return QStringLiteral("plan");
                    if (name.startsWith(QStringLiteral("get_")) ||
                        name == QLatin1String("tab_list"))
                        return QStringLiteral("terminal");
                    if (name == QLatin1String("tool_info") ||
                        name == QLatin1String("token_usage") ||
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
                        // whole tools/list response. Today every
                        // Required-contract tool declares the property
                        // via `makeCallerCwdReadProp` — this guard
                        // covers a future Required registration that
                        // skipped the property by mistake.
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

                // ANTS-1502 — two-tier discovery. When the caller
                // requests `_meta.shape == "lite"`, return a compact
                // shape `[{name, summary, kind}]` per tool (~80 chars
                // per entry) instead of the full descriptor set
                // (~60-75 KiB for ~50 tools). Callers drill into a
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
                    auto summaryFor = [](const QJsonObject &t) -> QString {
                        // Prefer selection_hint (already terse,
                        // form-factor cue). Fall back to the first
                        // sentence of description, truncated to
                        // ~140 chars to keep the lite shape compact.
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
                        e[QStringLiteral("summary")] = summaryFor(t);
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
                // ANTS-1356 — dispatchResult overrides the default
                // "ok" passed to recordDispatch. Refusal branches set
                // this to surface the refusal in mcp_trace +
                // token_usage's failed-call accumulator.
                // ANTS-1454 retrofitted the ANTS-1404 branch below to
                // set "caller_cwd_required" — the pre-1454 default-to-
                // ok masked the per-call cost of misconfigured callers
                // (see docs/specs/ANTS-1454.md).
                QString dispatchResult = QStringLiteral("ok");
                // ANTS-1360 — start latency clock before cache lookup
                // so cache-hit records capture true µs-range latency
                // (not zero). Stop right before recordMcpTrace below.
                QElapsedTimer mcpTraceTimer;
                mcpTraceTimer.start();
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
                const CallerCwdContract contract =
                    callerCwdContractFor(toolName);
                const QString callerCwd =
                    argsObj.value(QStringLiteral("caller_cwd"))
                        .toString();
                if (contract == CallerCwdContract::Required &&
                    callerCwd.isEmpty()) {
                    QJsonObject env;
                    env["ok"]    = false;
                    env["code"]  = QStringLiteral("caller_cwd_required");
                    env["error"] = QString(
                        "%1: caller_cwd is required for this tool "
                        "(ANTS-1404). Pass your $PWD as caller_cwd "
                        "so the tool routes to your project rather "
                        "than whichever tab Ants has focused.")
                        .arg(toolName);
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
                // ANTS-1356 — per-tool sliding-window rate-limit.
                // Runs AFTER caller_cwd_required (a misconfigured
                // caller should see the precise refusal first) but
                // BEFORE the idempotent-read cache lookup (cache
                // hits consume bucket budget — INV-5). Refusal sets
                // toolHandled=true with a {ok:false, code:"rate_limited",
                // retry_after_ms} envelope; downstream wrap + record
                // paths see it like any other completed call.
                if (!toolHandled) {
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
                const bool cacheable =
                    !toolHandled && isIdempotentReadTool(toolName);
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
                // external provider. All 12 outward-delegate tools
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
                        if (reqName.isEmpty()) {
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
                            }
                        }
                        responseText = QString::fromUtf8(
                            QJsonDocument(env).toJson(QJsonDocument::Compact));
                        toolHandled = true;
                    } else if (auto it = m_toolProviders.find(toolName);
                               it != m_toolProviders.end()) {
                        responseText = it->second(argsObj);
                        toolHandled = true;
                    }
                }
                // ANTS-1357 — populate cache on miss-success. INV-5
                // exclusions enforced inside maybeInsertIdempotentReadCache.
                // The cache stores the un-etagged response so subsequent
                // callers can either reuse it verbatim OR short-circuit
                // via etag_match against the same fingerprint.
                if (toolHandled && cacheable && !cachedHit) {
                    maybeInsertIdempotentReadCache(
                        toolName, argsObj, responseText);
                }
                // ANTS-1499 — ETag short-circuit. Runs after the
                // idempotent-read cache so the cached body stays in its
                // canonical (un-etagged) form, and before the wrap so
                // the etag is computed against the actual JSON envelope
                // (not the <ants_mcp_data> wrapper bytes which would
                // shift the hash for every tool name change).
                if (toolHandled && isEtagSupportedTool(toolName)) {
                    bool etagUnchanged = false;
                    responseText = applyEtagPattern(
                        toolName, argsObj, responseText, &etagUnchanged);
                    if (etagUnchanged) {
                        dispatchResult = QStringLiteral("etag_unchanged");
                    }
                }

                if (toolHandled) {
                    // ANTS-1294 — frame user-supplied content as data,
                    // not instructions. Control-plane tools (server-
                    // generated state) skip the wrap so a caller can
                    // syntactically distinguish structural metadata
                    // from content. See docs/specs/ANTS-1294.md.
                    const bool isControlPlane =
                        (toolName == QStringLiteral("get_session_info") ||
                         toolName == QStringLiteral("token_usage") ||
                         // ANTS-1399 — tool_info returns a descriptor
                         // slice (server-generated metadata about other
                         // tools), not user content. Bypass the wrap
                         // for the same reason get_session_info does.
                         toolName == QStringLiteral("tool_info"));
                    const QString wrapped = isControlPlane
                        ? responseText
                        : wrapMcpData(toolName, responseText);
                    result["content"] = makeTextContent(wrapped);
                    // ANTS-1284 — record dispatch (token_usage +
                    // mcp_trace). ANTS-1402-INV-3: now teed through
                    // a single recordDispatch hook so both observers
                    // see byte-identical numbers. ANTS-1284 byte-count
                    // contract preserved: arg/out bytes measure the
                    // wrapped payload (what actually crosses the wire).
                    // ANTS-1355: wrap delta + dispatch latency captured
                    // once at the dispatch site and forwarded verbatim.
                    const qint64 argBytes = QJsonDocument(argsObj)
                        .toJson(QJsonDocument::Compact).size();
                    const qint64 outBytes  = wrapped.toUtf8().size();
                    const qint64 rawBytes  = responseText.toUtf8().size();
                    const qint64 wrapBytes = outBytes - rawBytes;       // ANTS-1355 INV-3
                    const qint64 durUs     = mcpTraceTimer.nsecsElapsed() / 1000;
                    // ANTS-1356 + ANTS-1454 — dispatchResult is "ok"
                    // for normal success, "caller_cwd_required" for
                    // ANTS-1404 refusals, "rate_limited" for ANTS-1356
                    // refusals. recordDispatch derives `succeeded =
                    // (result == "ok")` so failed-call accounting in
                    // token_usage stays honest.
                    recordDispatch(toolName, argsObj, argBytes, outBytes,
                                   wrapBytes, durUs, cachedHit,
                                   dispatchResult);
                    haveResult = true;
                } else {
                    // JSON-RPC application error: tool not found or provider missing.
                    error["code"] = -32602; // Invalid params
                    error["message"] = QString("Unknown tool: %1").arg(toolName);
                    // ANTS-1402-INV-4 — failure-branch hook now routes
                    // through recordDispatch with result="tool_not_found".
                    // m_tokenUsage.recordCall is skipped inside
                    // recordDispatch when result != "ok" (preserves
                    // pre-1402 behaviour). INV-12 of ANTS-1360 still
                    // applies: resp_bytes=0 and a non-negative
                    // duration_us reach the mcp_trace ring.
                    const qint64 argBytes = QJsonDocument(argsObj)
                        .toJson(QJsonDocument::Compact).size();
                    const qint64 durUs = mcpTraceTimer.nsecsElapsed() / 1000;
                    recordDispatch(toolName, argsObj, argBytes,
                                   /*outBytes=*/0,
                                   /*wrapBytes=*/0,
                                   durUs,
                                   /*cachedHit=*/false,
                                   QStringLiteral("tool_not_found"));
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

// --- ANTS-1404 — per-tool caller_cwd contract ---

// Static classification table. Adding a new tool without an entry
// defaults to Optional (INV-4) — gracious degradation so a typo
// doesn't refuse calls. Required-tier entries are the four whose
// silent focused-fallback is the 2026-05-15 cross-session leak shape.
// See docs/specs/ANTS-1404.md for the full classification rationale.
// ANTS-1351 + ANTS-1397 § 2.4 — inline verb-in-flight gate
// implementations. Single QMutex around a QHash keyed by
// (verb, canonicalProjectRoot). Stale-slot reaper sweeps entries
// older than kVerbInFlightReapMs to recover from worker-death
// orphans (SIGKILL, segfault, panic-no-RAII).
qint64 ClaudeIntegration::verbInFlightTryAcquire(
        const QString &verb, const QString &projectRoot) {
    QMutexLocker lk(&m_verbInFlightMutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Stale-slot reap: drop any entries older than the reap window.
    for (auto it = m_verbInFlight.begin(); it != m_verbInFlight.end(); ) {
        if (now - it.value() > kVerbInFlightReapMs) {
            it = m_verbInFlight.erase(it);
        } else {
            ++it;
        }
    }
    const auto key = qMakePair(verb, projectRoot);
    auto it = m_verbInFlight.find(key);
    if (it != m_verbInFlight.end()) return it.value();  // already running
    m_verbInFlight.insert(key, now);
    return -1;
}

void ClaudeIntegration::verbInFlightRelease(
        const QString &verb, const QString &projectRoot) {
    QMutexLocker lk(&m_verbInFlightMutex);
    m_verbInFlight.remove(qMakePair(verb, projectRoot));
}

ClaudeIntegration::CallerCwdContract
ClaudeIntegration::callerCwdContractFor(const QString &toolName) {
    using C = CallerCwdContract;
    // Required — refuse with caller_cwd_required when absent.
    if (toolName == QStringLiteral("get_git_status"))     return C::Required;
    if (toolName == QStringLiteral("last_audit_summary")) return C::Required;
    if (toolName == QStringLiteral("git_state"))          return C::Required;
    if (toolName == QStringLiteral("verify_changes"))     return C::Required;
    // ANTS-1352: indie_review_dispatch — server-side reviewer fan-out.
    // Required: assembles per-lane prompts from caller_cwd's CLAUDE.md.
    if (toolName == QStringLiteral("indie_review_dispatch"))     return C::Required;
    // ANTS-1351 — audit_run mutates per-call SARIF files under /tmp
    // and consumes a worker-pool slot; Required gate refuses early
    // before any pool dispatch.
    if (toolName == QStringLiteral("audit_run"))          return C::Required;
    // ANTS-1430 — project_layout reads from the tenant-hashed
    // session_memory store. Joins session_memory in the gated
    // Required set (see ANTS-1336 INV-7 amendment).
    if (toolName == QStringLiteral("project_layout"))     return C::Required;
    // ANTS-1435 — session_memory: dispatcher refuses empty
    // caller_cwd upstream (Required). The handler still has a
    // body-level cwd_missing for the IPC path which bypasses the
    // contract — kept for diagnostic parity. Asymmetric internal
    // routing: reads anchor to caller_cwd, writes match focused tab.
    if (toolName == QStringLiteral("session_memory"))     return C::Required;
    // TabSpecific — classified but not enforced in Phase 3a. The
    // ANTS-1392 routing semantics (caller_cwd as a tab-routing key)
    // need their own spec pass before refusal makes sense.
    if (toolName == QStringLiteral("get_scrollback"))     return C::TabSpecific;
    if (toolName == QStringLiteral("get_text"))           return C::TabSpecific;
    if (toolName == QStringLiteral("get_last_command"))   return C::TabSpecific;
    if (toolName == QStringLiteral("get_environment"))    return C::TabSpecific;
    if (toolName == QStringLiteral("get_cwd"))            return C::TabSpecific;
    // ProcessGlobal — caller_cwd accepted and ignored.
    if (toolName == QStringLiteral("mcp_trace"))          return C::ProcessGlobal;
    if (toolName == QStringLiteral("token_usage"))        return C::ProcessGlobal;
    if (toolName == QStringLiteral("tab_list"))           return C::ProcessGlobal;
    if (toolName == QStringLiteral("get_session_info"))   return C::ProcessGlobal;
    // ANTS-1399 — tool_info reads the process-wide descriptor cache.
    if (toolName == QStringLiteral("tool_info"))          return C::ProcessGlobal;

    // ANTS-1424 — roadmap_log mutates ROADMAP.md + .roadmap-counter
    // under the caller's project root. Required-contract gated so
    // an absent caller_cwd refuses at the dispatcher rather than
    // falling back to the focused tab's roadmap.
    if (toolName == QStringLiteral("roadmap_log"))        return C::Required;
    // ANTS-1583 — roadmap_branch_drift: read-only ROADMAP scan +
    // git reachability check, anchored to the caller's project root.
    if (toolName == QStringLiteral("roadmap_branch_drift")) return C::Required;
    // ANTS-1400 — caller_cwd_info is the diagnostic verb that
    // surfaces the resolution Source enum. caller_cwd is its
    // INPUT (not an anchor), so neither Required nor ProcessGlobal
    // is the right fit — Optional accepts the empty case (which
    // is the "what would happen without it?" question the verb
    // is built to answer).
    if (toolName == QStringLiteral("caller_cwd_info"))    return C::Optional;

    // ANTS-1520 — promoted from Optional to Required across the
    // project-scoped read and review surfaces. The mixed regime
    // (some tools Required, others Optional with focused-tab
    // fallback) added a per-call mental tax of "did I need it
    // this time?" and, worse, optional `caller_cwd` quietly
    // returned the wrong project's data when the focused Ants
    // tab differed from the caller's project. Uniform refusal
    // closes that silent-miss failure mode. The shared
    // `caller_cwd_required` refusal envelope is emitted by the
    // tools/call dispatcher at the same site as ANTS-1404
    // (no per-tool wiring needed).
    if (toolName == QStringLiteral("roadmap_query"))      return C::Required;
    if (toolName == QStringLiteral("subsystem"))          return C::Required;
    if (toolName == QStringLiteral("workspace_search"))   return C::Required;
    if (toolName == QStringLiteral("file_outline"))       return C::Required;
    if (toolName == QStringLiteral("plan_template"))      return C::Required;
    // Cold-eyes verb cluster (ANTS-1313).
    if (toolName == QStringLiteral("cold_eyes_brief"))         return C::Required;
    if (toolName == QStringLiteral("cold_eyes_cross_doc_diff"))return C::Required;
    if (toolName == QStringLiteral("cold_eyes_fold_in"))       return C::Required;
    if (toolName == QStringLiteral("cold_eyes_partition"))     return C::Required;
    if (toolName == QStringLiteral("cold_eyes_single_doc"))    return C::Required;
    if (toolName == QStringLiteral("cross_doc_diff"))          return C::Required;
    // Debt-sweep verb cluster (ANTS-1316).
    if (toolName == QStringLiteral("debt_sweep_apply_fix"))     return C::Required;
    if (toolName == QStringLiteral("debt_sweep_defer"))         return C::Required;
    if (toolName == QStringLiteral("debt_sweep_scan"))          return C::Required;
    if (toolName == QStringLiteral("debt_sweep_triage_prompt")) return C::Required;
    // Indie-review verb cluster (ANTS-1311).
    if (toolName == QStringLiteral("indie_review_brief"))            return C::Required;
    if (toolName == QStringLiteral("indie_review_corroborate"))      return C::Required;
    if (toolName == QStringLiteral("indie_review_fold_in"))          return C::Required;
    if (toolName == QStringLiteral("indie_review_partition"))        return C::Required;
    if (toolName == QStringLiteral("indie_review_synthesis_prompt")) return C::Required;
    // Test-audit verb cluster (ANTS-1397).
    if (toolName == QStringLiteral("test_audit_brief"))            return C::Required;
    if (toolName == QStringLiteral("test_audit_fold_in"))          return C::Required;
    if (toolName == QStringLiteral("test_audit_partition"))        return C::Required;
    if (toolName == QStringLiteral("test_audit_synthesis_prompt")) return C::Required;

    // Unclassified — fall through. Future tools should be added
    // above; the ANTS-1417 coverage test fails the build if a
    // new registerToolProvider call has no matching branch here.
    // ANTS-1520 also flips the fall-through default to Required
    // so new tools fail-closed: any new registration that didn't
    // get an explicit classification refuses without caller_cwd
    // (loud, contributor-visible) instead of silently anchoring
    // to the focused tab.
    return C::Required;
}

// --- ANTS-1357 — MCP idempotent-read cache helpers ---

bool ClaudeIntegration::isIdempotentReadTool(const QString &toolName) {
    // Allowlist enforced at lookup AND insert (INV-4). Hardcoded; new
    // tools must opt in explicitly to make caching-eligibility a
    // deliberate decision per tool.
    return toolName == QStringLiteral("get_cwd")
        || toolName == QStringLiteral("get_environment")
        || toolName == QStringLiteral("tab_list")
        || toolName == QStringLiteral("last_audit_summary");
}

bool ClaudeIntegration::isEtagSupportedTool(const QString &toolName) {
    // ANTS-1499 — read tools that return JSON envelopes large enough
    // for the etag short-circuit to be worth it. Order mirrors the
    // priority list in the roadmap entry.
    return toolName == QStringLiteral("project_layout")
        || toolName == QStringLiteral("roadmap_query")
        || toolName == QStringLiteral("file_outline")
        || toolName == QStringLiteral("last_audit_summary")
        || toolName == QStringLiteral("get_environment")
        || toolName == QStringLiteral("tab_list")
        || toolName == QStringLiteral("subsystem")
        || toolName == QStringLiteral("git_state")
        // ANTS-1583 — roadmap_branch_drift response is small but the
        // drift list rarely changes between calls (ROADMAP mtime +
        // HEAD reachability snapshot), so the etag 304 round-trip
        // saves the full re-emit on stable repos.
        || toolName == QStringLiteral("roadmap_branch_drift");
}

QString ClaudeIntegration::etagFor(const QString &payload) {
    // Same hex16-of-sha256 shape as idempotentReadCacheKey for
    // operational sanity (one hash format across MCP internals).
    return QString::fromUtf8(QCryptographicHash::hash(
        payload.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString ClaudeIntegration::applyEtagPattern(const QString &toolName,
                                            const QJsonObject &args,
                                            const QString &responseText,
                                            bool *unchanged) {
    if (unchanged) *unchanged = false;
    if (!isEtagSupportedTool(toolName)) return responseText;
    const QString etag  = etagFor(responseText);
    const QString match = args.value(QStringLiteral("etag_match"))
        .toString();
    if (!match.isEmpty() && match == etag) {
        QJsonObject env;
        env[QStringLiteral("ok")]        = true;
        env[QStringLiteral("unchanged")] = true;
        env[QStringLiteral("etag")]      = etag;
        if (unchanged) *unchanged = true;
        return QString::fromUtf8(
            QJsonDocument(env).toJson(QJsonDocument::Compact));
    }
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseText.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Provider returned a non-JSON payload — return as-is.
        // ETag injection only applies to JSON-envelope tools.
        return responseText;
    }
    QJsonObject obj = doc.object();
    obj[QStringLiteral("etag")] = etag;
    return QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString ClaudeIntegration::idempotentReadCacheKey(
    const QString &toolName, const QJsonObject &args) {
    // INV-9 — args are part of the key. QJsonObject::toJson(Compact)
    // emits keys lexicographically (Qt6 internal), so the bytes are
    // stable across runs for the same arg set.
    QByteArray buf = toolName.toUtf8();
    buf.append('\0');
    buf.append(QJsonDocument(args).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(QCryptographicHash::hash(
        buf, QCryptographicHash::Sha256).toHex().left(16));
}

QString ClaudeIntegration::tryGetIdempotentReadCache(
    const QString &toolName, const QJsonObject &args) const {
    if (!isIdempotentReadTool(toolName)) return QString();
    const QString key = idempotentReadCacheKey(toolName, args);
    auto it = m_idempotentReadCache.find(key);
    if (it == m_idempotentReadCache.end()) return QString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - it->stampMs > kIdempotentReadTtlMs) {
        // INV-2 — entries past TTL are treated as miss (the next
        // insert will overwrite).
        return QString();
    }
    // INV-3 LRU bump.
    m_idempotentReadLru.removeOne(key);
    m_idempotentReadLru.prepend(key);
    return it->response;
}

void ClaudeIntegration::maybeInsertIdempotentReadCache(
    const QString &toolName, const QJsonObject &args,
    const QString &response) {
    // INV-4 — allowlist check at insert.
    if (!isIdempotentReadTool(toolName)) return;
    // INV-5(a) — empty response = "couldn't answer". Don't cache.
    if (response.isEmpty()) return;
    // INV-5(b) — kRcUnavailable = transient startup window. Don't cache.
    if (response == QString::fromUtf8(kMcpRcUnavailable)) return;
    const QString key = idempotentReadCacheKey(toolName, args);
    m_idempotentReadCache.insert(key,
        IdempotentReadEntry{QDateTime::currentMSecsSinceEpoch(), response});
    m_idempotentReadLru.removeOne(key);
    m_idempotentReadLru.prepend(key);
    while (m_idempotentReadLru.size() > kIdempotentReadCacheCap) {
        const QString evicted = m_idempotentReadLru.takeLast();
        m_idempotentReadCache.remove(evicted);
    }
}

// --- ANTS-1356 — MCP per-tool rate-limit / quota ---

namespace {
// Test-only cap overrides. Defaulted to -1 = "use compile-time
// constants" (kRateLimitCheapCap / kRateLimitExpensiveCap). The
// `*ForTest` setters write here; production callers never touch it.
int g_rateLimitCheapCapOverride     = -1;
int g_rateLimitExpensiveCapOverride = -1;
}  // namespace

ClaudeIntegration::RateLimitClass
ClaudeIntegration::rateLimitClassFor(const QString &toolName) {
    using R = RateLimitClass;
    // ControlPlane — uncapped. Pure reads of server-owned state +
    // diagnostic verbs. Spec INV-13.
    if (toolName == QStringLiteral("get_session_info"))   return R::ControlPlane;
    if (toolName == QStringLiteral("token_usage"))        return R::ControlPlane;
    if (toolName == QStringLiteral("tool_info"))          return R::ControlPlane;
    if (toolName == QStringLiteral("mcp_trace"))          return R::ControlPlane;
    if (toolName == QStringLiteral("caller_cwd_info"))    return R::ControlPlane;
    // Expensive — 10/min. Heavy verbs (shell-out, subagent dispatch,
    // cmake/ctest, full-corpus scan).
    if (toolName == QStringLiteral("audit_run"))                return R::Expensive;
    if (toolName == QStringLiteral("workspace_search"))         return R::Expensive;
    if (toolName == QStringLiteral("verify_changes"))           return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_brief"))          return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_partition"))      return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_cross_doc_diff")) return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_fold_in"))        return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_single_doc"))     return R::Expensive;
    if (toolName == QStringLiteral("cross_doc_diff"))           return R::Expensive;
    if (toolName == QStringLiteral("indie_review_brief"))       return R::Expensive;
    if (toolName == QStringLiteral("indie_review_partition"))   return R::Expensive;
    if (toolName == QStringLiteral("indie_review_corroborate")) return R::Expensive;
    if (toolName == QStringLiteral("indie_review_fold_in"))     return R::Expensive;
    // ANTS-1352: indie_review_dispatch
    if (toolName == QStringLiteral("indie_review_dispatch"))    return R::Expensive;
    if (toolName == QStringLiteral("test_audit_brief"))         return R::Expensive;
    if (toolName == QStringLiteral("test_audit_partition"))     return R::Expensive;
    if (toolName == QStringLiteral("test_audit_fold_in"))       return R::Expensive;
    if (toolName == QStringLiteral("debt_sweep_scan"))          return R::Expensive;
    if (toolName == QStringLiteral("debt_sweep_apply_fix"))     return R::Expensive;
    // Default — Cheap (60/min). All read verbs not classified above.
    return R::Cheap;
}

ClaudeIntegration::RateLimitTier
ClaudeIntegration::rateLimitTierFor(RateLimitClass cls) {
    switch (cls) {
        case RateLimitClass::ControlPlane:
            return RateLimitTier{INT_MAX, kRateLimitWindowMs};
        case RateLimitClass::Expensive: {
            const int cap = (g_rateLimitExpensiveCapOverride >= 0)
                ? g_rateLimitExpensiveCapOverride
                : kRateLimitExpensiveCap;
            return RateLimitTier{cap, kRateLimitWindowMs};
        }
        case RateLimitClass::Cheap:
        default: {
            const int cap = (g_rateLimitCheapCapOverride >= 0)
                ? g_rateLimitCheapCapOverride
                : kRateLimitCheapCap;
            return RateLimitTier{cap, kRateLimitWindowMs};
        }
    }
}

qint64 ClaudeIntegration::rateLimitCheck(
    const QString &toolName, const QString &callerCwd, qint64 nowMs) {
    const RateLimitClass cls = rateLimitClassFor(toolName);
    if (cls == RateLimitClass::ControlPlane) return 0;
    const RateLimitTier tier = rateLimitTierFor(cls);

    const QPair<QString, QString> key{toolName, callerCwd};
    auto it = m_rateLimitBuckets.find(key);

    // Map-cap LRU eviction (INV-7). Only when inserting a NEW key
    // at the cap; existing-key updates don't grow the map.
    if (it == m_rateLimitBuckets.end() &&
        m_rateLimitBuckets.size() >= kRateLimitMapCap) {
        // Linear scan: evict bucket with the oldest most-recent
        // timestamp (LRU-by-last-call). Bounded cost (256 ≤ 256).
        QPair<QString, QString> evictKey;
        qint64 evictMostRecent = std::numeric_limits<qint64>::max();
        for (auto cit = m_rateLimitBuckets.constBegin();
             cit != m_rateLimitBuckets.constEnd(); ++cit) {
            const qint64 mostRecent =
                cit->tsMs.isEmpty() ? 0 : cit->tsMs.back();
            if (mostRecent < evictMostRecent) {
                evictMostRecent = mostRecent;
                evictKey = cit.key();
            }
        }
        m_rateLimitBuckets.remove(evictKey);
    }

    RateLimitBucket &bucket = m_rateLimitBuckets[key];

    // Prune front: drop entries that fell out of the sliding window.
    while (!bucket.tsMs.isEmpty() &&
           bucket.tsMs.front() < nowMs - tier.windowMs) {
        bucket.tsMs.dequeue();
    }

    // INV-6 — empty bucket auto-evicts. Insert nowMs into a fresh
    // entry and return accept. (We re-fetch the bucket reference
    // after the remove + re-insert because QHash may rehash.)
    if (bucket.tsMs.isEmpty()) {
        m_rateLimitBuckets.remove(key);
        RateLimitBucket &fresh = m_rateLimitBuckets[key];
        fresh.tsMs.enqueue(nowMs);
        return 0;
    }

    // Under cap — accept and append.
    if (bucket.tsMs.size() < tier.capPerWindow) {
        bucket.tsMs.enqueue(nowMs);
        return 0;
    }

    // At cap — refuse. retry_after_ms = (oldest + window) - now,
    // floored at 1 (callers expect a positive sentinel).
    const qint64 retryAfter = (bucket.tsMs.front() + tier.windowMs) - nowMs;
    return retryAfter > 0 ? retryAfter : 1;
}

void ClaudeIntegration::setRateLimitCapsForTest(int cheap, int expensive) {
    g_rateLimitCheapCapOverride     = cheap;
    g_rateLimitExpensiveCapOverride = expensive;
}

void ClaudeIntegration::resetRateLimitCapsForTest() {
    g_rateLimitCheapCapOverride     = -1;
    g_rateLimitExpensiveCapOverride = -1;
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

