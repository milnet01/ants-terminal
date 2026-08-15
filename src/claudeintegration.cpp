#include "claudeintegration.h"

#include "build_info.h"  // ANTS-1952 — git SHA + build time for serverInfo
#include "configpaths.h"
#include "debuglog.h"
#include "mcpprojection.h"
#include "mcpspill.h"          // ANTS-2094 — result offload
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
#include <QPointer>  // ANTS-2101 — guard MCP socket across nested-loop dispatch
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

// ANTS-1415 — the CallerCwdContract::TabSpecific tools whose handler
// honours an explicit `tab` index as an alternate routing key
// (cmdGetText, cmdRecentErrors, cmdLastSelection). The other four
// (get_scrollback, get_last_command, get_environment, get_cwd) ignore
// `tab`, so it must NOT count as a routing key for them — else a stray
// tab:N would bypass the Phase 3b gate and still fall back to the
// focused tab. See docs/specs/ANTS-1415.md.
static bool tabSpecificAcceptsTabIndex(const QString &toolName) {
    return toolName == QLatin1String("get_text") ||
           toolName == QLatin1String("recent_errors") ||
           toolName == QLatin1String("last_selection");
}

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
    // True when `pid`'s argv resolves to Claude Code — a direct
    // `claude`/`claude-code` binary, or a node/deno/bun launcher running
    // the claude script. Used inline by both the fast path and the
    // fallback so each returns on the FIRST matching child rather than
    // building a full child list and re-looping (ANTS-1845: bounds the
    // fallback's per-tab 2 s /proc scan).
    auto isClaudePid = [&](pid_t pid) -> bool {
        QFile cmdFile(QString("/proc/%1/cmdline").arg(pid));
        if (!cmdFile.open(QIODevice::ReadOnly)) return false;
        QByteArray raw = cmdFile.readAll();
        cmdFile.close();
        QList<QByteArray> argv = raw.split('\0');
        while (!argv.isEmpty() && argv.last().isEmpty()) argv.removeLast();
        if (argv.isEmpty()) return false;

        QString arg0 = basename(QString::fromUtf8(argv.first()));
        if (isClaudeBin(arg0)) return true;

        // Node/deno/bun launchers: inspect argv[1..] for a script basename
        // or a path containing "/claude/" or "/claude-code/".
        if (arg0 == QLatin1String("node") || arg0 == QLatin1String("deno") ||
            arg0 == QLatin1String("bun")) {
            for (qsizetype i = 1; i < argv.size(); ++i) {
                QString scriptName = basename(QString::fromUtf8(argv[i]));
                QString full = QString::fromUtf8(argv[i]);
                if (isClaudeBin(scriptName) ||
                    full.contains(QLatin1String("/claude-code/")) ||
                    full.contains(QLatin1String("/claude/")))
                    return true;
            }
        }
        return false;
    };

    // Fast positive path: the kernel's `children` files list this shell's
    // direct children (much cheaper than scanning all /proc). The file is
    // per-THREAD, not per-process — a child forked by a non-leader thread
    // appears under that thread's task entry, never the leader's — so union
    // over every /proc/<pid>/task/<tid>/children. On a hit, return early.
    //
    // A miss here is NOT authoritative: a child whose forking thread has
    // since EXITED is orphaned from every task/<tid>/children (the tid is
    // gone) yet still has ppid == this process. That is exactly Qt's
    // QProcess launcher behaviour on some builds — it forks the child from
    // a transient internal thread that then dies — which left
    // ClaudeTranscriptRobustness inv7 red on CI when the union short-cut
    // skipped the scan (ANTS-1867). So always fall through to the ppid
    // scan below, which is the complete (if pricier) view. Real shells are
    // single-threaded and long-lived, so production claude detection hits
    // the union fast path and never pays the scan.
    QDir taskDir(QString("/proc/%1/task").arg(shellPid));
    for (const QString &tid : taskDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile childFile(QString("/proc/%1/task/%2/children").arg(shellPid).arg(tid));
        if (!childFile.open(QIODevice::ReadOnly)) continue;
        const QString children = QString::fromUtf8(childFile.readAll()).trimmed();
        childFile.close();
        for (const QString &pidStr : children.split(' ', Qt::SkipEmptyParts)) {
            bool ok;
            pid_t pid = pidStr.toInt(&ok);
            if (ok && pid > 0 && isClaudePid(pid)) return pid;
        }
    }

    // Authoritative fallback: scan /proc, checking each ppid match's binary
    // inline. Catches orphaned children (forking thread gone) and kernels /
    // containers that don't expose /proc/<pid>/task/<tid>/children at all.
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
        if (fields[1].toInt() != shellPid) continue;
        if (isClaudePid(pid)) return pid;
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

    // ANTS-1845: the read of /proc/<pid>/stat IS the liveness check — a
    // dead pid fails to open and returns 0. The narrow window where pid
    // was reused between findClaudeChildPid and here is benign: callers
    // feed the result to sessionPathForCwd as the freshness FLOOR
    // (minLastEventMs). A reused pid yields a NEWER start time, i.e. a
    // STRICTER floor, so the worst case is a false-negative (no transcript
    // bound) that self-heals on the next 2 s poll once the correct pid is
    // detected. It can never bind another project's transcript (scoping is
    // by cwd, and the floor only moves up), so no extra cross-check is
    // warranted.
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
// ANTS-2191 — when a live PID anchor exists (requireContentTs, threaded from
// minLastEventMs > 0 at the call site) we require a content timestamp: file
// mtime is same-UID-spoofable (ADR-0004 integrity smell), so falling back to
// it could let a tampered transcript's mtime pass the freshness filter and
// bind the wrong session (narrowed re-open of the ANTS-1163 wrong-session
// bind). Returning 0 makes the candidate fail filter (a) in sessionPathForCwd
// (0 < minLastEventMs - kLeewayMs). With no PID anchor the mtime fallback is
// the only freshness signal we have, so it is retained.
static qint64 effectiveLastEventMs(const QFileInfo &fi, bool requireContentTs) {
    const qint64 fromContent =
        ClaudeIntegration::lastEventTimestampMs(fi.absoluteFilePath());
    if (fromContent > 0) return fromContent;
    if (requireContentTs) return 0;  // reject: no trustworthy timestamp
    return fi.lastModified().toMSecsSinceEpoch();
}

// ANTS-1338 (lane-3 H2) — PID-reuse contract. This function NEVER trusts a
// process identity: `minLastEventMs` is a process-start FLOOR (the epoch-ms
// when the live Claude PID started, per processStartTimeMs / ANTS-1845), not a
// PID and not a claim about which process owns the transcript. Linux PID reuse
// is therefore benign here: a reused (non-Claude) PID only yields a NEWER start
// time, i.e. a STRICTER floor, which can only reject more candidates — it can
// never surface another process's or a dead session's transcript (selection is
// cwd-scoped and the floor moves monotonically up). The residual integrity gap
// (mtime spoofing of the freshness signal) is closed by ANTS-2191's
// requireContentTs in effectiveLastEventMs. Callers must keep passing a
// start-time floor here, not a trusted PID — do not "optimise" this into a
// direct PID/argv check.
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
                const qint64 effMs = effectiveLastEventMs(fi, minLastEventMs > 0);
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

    // ANTS-1806 — cap per-line read. The transcript is written by an external
    // process (Claude Code) and is an untrusted parse boundary; a corrupt
    // multi-GiB single line would otherwise OOM the process even under the
    // whole-file guard above. Matches extractCwdFromTranscript's cap.
    constexpr qint64 kMaxLineBytes = 64 * 1024;
    while (!file.atEnd()) {
        QByteArray line = file.readLine(kMaxLineBytes).trimmed();
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

    // Sub-agent (sidechain) events are interleaved in the same transcript but
    // belong to a child task, not the main session. A sidechain end_turn must
    // not flip the main session to Idle while the outer Task tool_use is still
    // in flight. Mirror the filter already present in claudebgtasks.cpp.
    QList<QJsonObject> events;
    for (const QByteArray &raw : tail.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;
        const QJsonObject obj = doc.object();
        if (obj.value(QStringLiteral("isSidechain")).toBool()) continue;
        events.append(obj);
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
            // ANTS-1659 — hook events are <8 KiB; cap at 256 KiB to close a
            // same-UID OOM vector (deeply-nested JSON balloons 3-5× in QJson
            // tree allocation). Was 10 MiB.
            if (buf.size() > 256 * 1024) { socket->disconnectFromServer(); return; }
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
// ANTS-1419: a CallerCwdContract value is now part of every
// registration so the per-tool security classification lives next to
// the tool's handler in mainwindow.cpp rather than in a separate
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
    return out;
}

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
    ToolHandler wrapped =
        [name, inner = std::move(handler)]
        (const QJsonObject &args) -> QString {
            ANTS_LOG(DebugLog::Claude,
                     "mcp lambda-enter tool=%s arg_keys=%lld",
                     name.toUtf8().constData(),
                     static_cast<long long>(args.size()));
            return inner(args);
        };
    m_toolProviders[name] = RegisteredTool{std::move(wrapped), contract};
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
    const bool succeeded = (result == QLatin1String("ok"));
    m_tokenUsage.recordCall(toolName, argBytes, outBytes,
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
    emit tokensSavedUpdated(m_tokenUsage.buildReport(false).totalSaved);
}

void ClaudeIntegration::endTokenSession() {
    // ANTS-3572 — sole reset path (see docs/specs/ANTS-3572.md). Emit BEFORE
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

void ClaudeIntegration::onMcpConnection() {
    while (m_mcpServer->hasPendingConnections()) {
        QLocalSocket *socket = m_mcpServer->nextPendingConnection();
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
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, idleTimer]() {
            if (socket->property("_handled").toBool()) return;
            QByteArray buf = socket->property("_buf").toByteArray();
            buf += socket->readAll();
            // ANTS-1659 — MCP requests are small (tool name + args); cap at
            // 256 KiB to close the same nested-JSON OOM vector as the hook
            // path. Was 10 MiB.
            if (buf.size() > 256 * 1024) { socket->disconnectFromServer(); return; }
            socket->setProperty("_buf", buf);
            QJsonDocument doc = QJsonDocument::fromJson(buf);
            if (!doc.isObject()) return; // wait for more data
            socket->setProperty("_handled", true);

            // ANTS-2101 — a complete request is in hand: stop the 5 s
            // slow-loris idle timer BEFORE dispatching. A tool dispatch
            // (audit_run et al.) can run a nested event loop that pumps
            // QProcesses; a still-armed timer would fire timeout ->
            // socket->abort() -> disconnected -> deleteLater(), and that
            // deleteLater is processed BY the nested loop — freeing this
            // socket before the write at the tail. Mirrors the
            // remotecontrol.cpp ANTS-2026 fix for the identical pattern.
            idleTimer->stop();
            // Defence in depth: the peer can still disconnect mid-dispatch,
            // freeing the socket via the same disconnected -> deleteLater
            // chain. A QPointer lets the post-dispatch write bail instead of
            // touching a dangling pointer.
            QPointer<QLocalSocket> guard(socket);

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
                // ANTS-3572 — fold the previous session into the persisted
                // aggregate before clearing (endTokenSession emits first).
                endTokenSession();
                QJsonObject caps;
                caps["tools"] = QJsonObject();
                QJsonObject serverInfo;
                serverInfo["name"] = "ants-terminal";
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

                // ANTS-1720 — `fields` projection input prop. The 11
                // high-volume read tools in mcp::isFieldProjectionTool
                // (roadmap_query, project_layout, file_outline,
                // get_environment, tab_list, subsystem, git_state, read_log,
                // read_region, codebase_index, model_switch_stats) accept it;
                // the response carries only the named top-level fields. Gated
                // by that helper at the dispatch site.
                auto makeFieldsProp = []{
                    QJsonObject p;
                    p["type"] = "array";
                    QJsonObject items;
                    items["type"] = "string";
                    p["items"] = items;
                    p["description"] = QStringLiteral(
                        "Optional. Return only these top-level response "
                        "fields (e.g. [\"bullets\"], [\"branch\","
                        "\"files\"]). Unknown names are ignored; an "
                        "all-unknown list yields {}. Omit for the full "
                        "payload — fully backwards-compatible (ANTS-1720). "
                        "To keep the etag for a follow-up 304 call, "
                        "include \"etag\" in the list: the etag is "
                        "computed on the unfiltered body, so a narrowed "
                        "call still short-circuits when state is "
                        "unchanged (composes with etag_match).");
                    return p;
                };

                // ANTS-2091 — `compact` opt-in flag, added alongside
                // `fields` on the same read tools. When true, the response
                // is stripped of dead-weight fields (null / false / "" /
                // [] / {}) the model never reads; protected keys (ok, code,
                // error, etag, found, unchanged) always survive.
                auto makeCompactProp = []{
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
                };

                // ANTS-2218 — `raw` opt-in flag on the content-read verbs
                // (read_region / read_regions / workspace_search). When true
                // the body is returned VERBATIM inside an unforgeable nonce
                // frame instead of the default scrub that neutralises literal
                // </ants_mcp_data> and `<!--`/`-->` markers — so an agent
                // reading frame-sensitive source (this file, a spec, an HTML/
                // markdown file with comments) gets true bytes to Edit from.
                auto makeRawProp = []{
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
                };

                // ANTS-2227 — uniform `dry_run` flag for mutating verbs. When
                // true the verb computes the would-be result envelope (carrying
                // dry_run:true) WITHOUT writing to disk. The per-verb
                // descriptions on roadmap_log / changelog_log / spec_log
                // (ANTS-2077 / 2136) predate this factory and keep their
                // tailored copies; new write verbs share this one.
                auto makeDryRunProp = []{
                    QJsonObject p;
                    p["type"] = "boolean";
                    p["default"] = false;
                    p["description"] = QStringLiteral(
                        "Optional (ANTS-2227). When true, return the would-be "
                        "result (carrying `dry_run:true`) WITHOUT writing to "
                        "disk — a free pre-flight that shares the real write "
                        "path, so the preview can't drift from the actual "
                        "result. Defaults false.");
                    return p;
                };

                // ANTS-3498 — optional id_prefix override for the three
                // fold-in verbs (test_audit / cold_eyes / indie_review_fold_in),
                // parity with roadmap_log op:append's id_prefix. Same grammar
                // (RoadmapFoldIn::isValidIdPrefix / ANTS-3492): 1-16 chars of
                // [A-Za-z0-9_-] containing ≥1 letter.
                auto makeFoldInIdPrefixProp = []{
                    QJsonObject p;
                    p["type"] = "string";
                    p["pattern"] =
                        "^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]{0,15}$";
                    p["description"] = QStringLiteral(
                        "Optional (ANTS-3498). Pin the ID prefix used for the "
                        "folded-in bullets instead of sniffing it from the "
                        "target ROADMAP.md — parity with roadmap_log op:append. "
                        "Must contain a letter and be 1-16 chars of "
                        "[A-Za-z0-9_-] (e.g. ANTS, 3D_E). When omitted, the "
                        "project's dominant [PREFIX-NNNN] prefix is sniffed "
                        "(fallback \"ANTS\" on a greenfield roadmap).");
                    return p;
                };

                // ANTS-2090 — `encoding` selector, declared on the
                // list-shaped read verbs. "tabular" packs each eligible
                // top-level array-of-objects into a columnar
                // {__cols__,__rows__} form (one header row + one value-row
                // per element), 30–60% smaller on big lists. Opt-in only —
                // the caller must decode it — so there is no session default.
                auto makeEncodingProp = []{
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
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
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
                // ANTS-3409 — the wire one-liner overran its 800 B budget
                // (mcp_tool_detail_field INV-5) by 14 B; the `bundles` mode
                // gloss said "bundle" twice, so its redundant middle was
                // trimmed (full per-op detail is in `detail` below / via
                // tool_info {name:"roadmap_query"}).
                roadmapTool["description"] = QStringLiteral(
                    "Query ROADMAP.md as structured bullets {id, status, "
                    "headline, headline_oneline, kind, lanes}. Filters: "
                    "status (all|active|shipped), section=<slug>, "
                    "query=<keyword>, id / "
                    "ids[] (by [PROJ-NNNN]). mode: "
                    "bullets (default) | section_index (slug discovery) | "
                    "headline_only (~10x smaller) | bundles (thematic "
                    "work-bundles). Opt-in: include_body, "
                    "compact, fields, etag_match. Refusals: bad_case, "
                    "bad_section, bad_mode_combo. caller_cwd Required. "
                    "`source` = the backend that answered, not `path` "
                    "(ANTS-4402).");
                // ANTS-2079 — full per-op reference lives in `detail`,
                // stripped from the tools/list wire and served on demand
                // by tool_info {name:"roadmap_query"}.
                roadmapTool["detail"] = QStringLiteral(
                    "ANTS-4402 — WHICH BACKEND ANSWERED. The envelope "
                    "carries `source`: \"store\" or \"markdown\". `path` "
                    "names ROADMAP.md on BOTH, so it does not tell you. On "
                    "a migrated project the store is authoritative and an "
                    "edit made to ROADMAP.md by hand is NOT reflected here "
                    "— it will not appear in bullets, and an id filed that "
                    "way comes back in `missing_ids`. "
                    "`file_ahead_of_store:true` (with `file_highest_id` and "
                    "`store_high_water`) means the file holds ids the store "
                    "has never seen, so the answer is PROVABLY stale. It is "
                    "one-directional: absence is not proof of freshness, "
                    "because a status flip or a body edit moves no id. "
                    "`query=<keyword>` is a CASE-INSENSITIVE substring "
                    "match over headline AND body; `id` fetches one bullet "
                    "and `ids[]` a bundle, both by [PROJ-NNNN]. (Both "
                    "glosses moved here from the wire description when "
                    "ANTS-4402 added `source` — the tools/list budget is "
                    "800 B and every session pays it, so the wire keeps "
                    "the names and this keeps the semantics.) "
                    "Query the active tab's ROADMAP.md as structured "
                    "bullets. Each bullet: {id, status, headline, "
                    "headline_oneline, kind, lanes}. `headline_oneline` "
                    "(ANTS-1521) is `headline` with newlines + "
                    "whitespace runs collapsed to a single space — safe "
                    "to concatenate into a summary without post-"
                    "processing. When the parser capped a long headline at "
                    "120 chars, the bullet also carries `headline_full` "
                    "(ANTS-2075) — the untruncated text, usable as a "
                    "roadmap_log headline locator. Optional "
                    "`include_body:true` "
                    "(ANTS-1517) adds a `body` field (truncated to "
                    "~2000 chars, `body_truncated:true` set on "
                    "truncation; a truncated body keeps its head AND "
                    "its tail either side of an explicit elision "
                    "marker — ANTS-3736) — saves the 3-5 follow-up Reads a "
                    "session does to pick up Kind / Lanes / Source "
                    "prose from a dense bundle table. Optional "
                    "`status` filter — \"active\" "
                    "(📋+🚧, ~1.7 K tokens — recommended for planning "
                    "queries) / \"shipped\" (✅ only) / \"all\" (default, "
                    "~12 K tokens). Optional `section` slug — returns "
                    "only bullets within that ## or ### heading "
                    "(e.g. \"performance\", \"080\"); response carries "
                    "`section` echo. Optional `id` — fetch ONE bullet "
                    "by its [PROJ-NNNN] id (e.g. \"ANTS-1853\") in a "
                    "single call instead of paging; bypasses status + "
                    "pagination, includes the body by default, returns "
                    "{ok, bullets, count, id, found} (ANTS-1856). "
                    "Optional `ids` array — plural sibling of `id` for "
                    "N-bullet bundle fetches in one call; same bypass + "
                    "body-by-default, envelope adds matched_ids/"
                    "missing_ids accounting (ANTS-1726). Max 100 ids. "
                    "ANTS-1696 — section= empties also carry a "
                    "`section_shape` (\"table\"|\"prose\") + "
                    "`non_bullet_lines` hint when the slice has "
                    "non-bullet content (e.g. a planning table or prose "
                    "block); lets the caller skip a raw Read fallback. "
                    "Absent on bullet-rich and truly-empty sections "
                    "(back-compat). "
                    "Optional `mode` — \"bullets\" "
                    "(default) / \"section_index\" (returns a compact "
                    "{slug, headline, level, active_count, "
                    "shipped_count, total_count, active_count_id_only, "
                    "shipped_count_id_only, total_count_id_only}[] "
                    "index instead of bullets[] — use for slug discovery "
                    "before drilling in via section=; honours `status` so "
                    "status:\"active\" lists only sections with active work "
                    "(ANTS-1848)) / \"headline_only\" (ANTS-1881 — "
                    "bullets[] narrowed to id + status + headline_oneline "
                    "+ section_slug per bullet, ~10× smaller payload on "
                    "dense bundle sections; composes with section=, "
                    "status=, pagination, ETag). The `*_id_only` "
                    "parallels (ANTS-1622) count only bullets that "
                    "carry a [PROJ-NNNN] id, matching the default "
                    "bullets[] predicate — when `active_count > "
                    "active_count_id_only`, the section has legacy "
                    "narrator-prose entries that would be invisible to "
                    "a default `bullets[]` query; pass "
                    "include_narrator_bullets:true to retrieve them. "
                    "The envelope also surfaces a top-level "
                    "`legacy_format_sections[]` array listing slugs "
                    "where every direct bullet is narrator-only; the "
                    "same sections carry a per-section `legacy_format:"
                    "true` flag (ANTS-1714b) so you can spot them "
                    "without grepping the slug against the array. "
                    "ANTS-2052 — on a FULLY id-less roadmap status:active/"
                    "shipped would otherwise drop every section (all "
                    "*_id_only:0) and return sections:[]; the branch then "
                    "falls back to the raw emoji count so sections still "
                    "list, and the envelope carries top-level "
                    "`legacy_format:true` + `raw_active_count` + "
                    "`raw_shipped_count` (emoji-based) so a session_orient "
                    "bundle no longer reads as an empty queue. "
                    "ANTS-1646 — every non-error response also carries "
                    "a top-level `duplicate_ids[]` field when the parser "
                    "saw the same canonical `[PROJ-NNNN]` id on more "
                    "than one bullet (each entry: `{id, occurrences:"
                    "[{section_slug, status}], truncated_count?}`). "
                    "ANTS-1688 — the detector keys only on canonical "
                    "allocated IDs, so synthetic content-hash nonces "
                    "and Obsidian `^anchor` tokens on legacy roadmaps "
                    "no longer masquerade as collisions; `occurrences[]` "
                    "is capped at 3 with the dropped tail in "
                    "`truncated_count`. Absent when the roadmap is "
                    "clean. Surfaces hand-edited drift past the "
                    "`.roadmap-counter` guard `roadmap_log` maintains "
                    "so the next session sees the collision instead of "
                    "inheriting a stale bullet at random. "
                    "Envelope: {ok, bullets, path, "
                    "count, filter, section?, mode?, duplicate_ids?} or "
                    "{ok, sections, path, filter, mode, "
                    "legacy_format_sections?, legacy_format_hint?, "
                    "duplicate_ids?} for "
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
                    statusEnum.append("planned");
                    statusEnum.append("in-progress");
                    statusEnum.append("considered");
                    statusProp["enum"] = statusEnum;
                    statusProp["description"] = QStringLiteral(
                        "Filter by lifecycle. Aggregates: \"active\" = "
                        "planned + in-progress (📋+🚧, ~7× smaller payload), "
                        "\"shipped\" = ✅, \"all\". ANTS-3400 — the granular "
                        "roadmap_log lifecycle names are also accepted and "
                        "map to a single emoji: \"planned\"=📋, "
                        "\"in-progress\"=🚧, \"considered\"=💭 (bullets / "
                        "section= path; section_index collapses granular "
                        "names to their aggregate). An unknown value refuses "
                        "with bad_status + an `accepted` list. ANTS-3698 — "
                        "`filter` is accepted as an alias (the envelope "
                        "echoes the applied lifecycle under that name, so it "
                        "is what a caller writing the next call by hand "
                        "sends); `status` wins when both are present.");
                    props["status"] = statusProp;
                    // ANTS-3698 — `filter` alias. Declared so the arg is not
                    // reported in ignored_args now that it is honoured, and so
                    // the echo field's name resolves to a real parameter.
                    QJsonObject filterAliasProp;
                    filterAliasProp["type"] = "string";
                    filterAliasProp["enum"] = statusEnum;
                    filterAliasProp["description"] = QStringLiteral(
                        "Alias for `status` (ANTS-3698) — same accepted "
                        "values. Exists because the response echoes the "
                        "applied lifecycle as `filter`; passing that name "
                        "used to be silently ignored and answered with the "
                        "full set. Prefer `status`, which wins if both are "
                        "sent.");
                    props["filter"] = filterAliasProp;
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
                    // ANTS-1856 — `id` single-item selector. The
                    // one-call answer to \"show me ANTS-NNNN\" without
                    // paging the whole roadmap.
                    QJsonObject idProp;
                    idProp["type"] = "string";
                    idProp["description"] = QStringLiteral(
                        "Fetch a single bullet by its [PROJ-NNNN] id "
                        "(e.g. \"ANTS-1853\") in one call instead of "
                        "paging. Returns {ok, bullets:[the item], "
                        "count, id, found}. Bypasses the `status` "
                        "filter + pagination (an id request wants THAT "
                        "item regardless of lifecycle), and includes "
                        "the body by default (pass include_body:false "
                        "to drop it). Case-sensitive exact match; a "
                        "case-only mismatch → code=bad_case with "
                        "`canonical_id`. Unknown id → {ok:true, "
                        "found:false, count:0}. Cannot combine with "
                        "`section` or mode:section_index "
                        "(bad_mode_combo).");
                    props["id"] = idProp;
                    // ANTS-1726 — `ids` plural-selector. The N-call
                    // answer to "show me ANTS-1719..1724" in one go;
                    // pairs with the singular `id` selector for
                    // bundle-continuation sessions. Document-order
                    // result + matched/missing accounting.
                    QJsonObject idsProp;
                    idsProp["type"] = "array";
                    {
                        QJsonObject items;
                        items["type"] = "string";
                        idsProp["items"] = items;
                    }
                    idsProp["maxItems"] = 100;
                    idsProp["description"] = QStringLiteral(
                        "Fetch N bullets by their [PROJ-NNNN] ids "
                        "(e.g. [\"ANTS-1719\",\"ANTS-1721\"]) in one "
                        "call instead of N separate id= calls or a "
                        "status:all scan. Bypasses `status` filter + "
                        "pagination (same as singular `id`), keeps body "
                        "by default. Result is in DOCUMENT order, not "
                        "input order. Envelope: {ok, bullets, count, "
                        "ids, matched_ids, missing_ids, found}. "
                        "ANTS-4400: each bullet carries `input_index` — its "
                        "position in the `ids` array you sent — so a caller "
                        "can restore its own ordering. Results stay in "
                        "DOCUMENT order; zipping them against your input array "
                        "without re-keying silently mis-pairs. "
                        "ANTS-4387: an id ROTATED into a docs/roadmap/*.md "
                        "archive (roadmap-format.md § 3.9) is reported in "
                        "`archived_ids` with its source in `archived_sources`, "
                        "NOT in `missing_ids` — those two were previously "
                        "indistinguishable, and for a release pre-flight or a "
                        "changelog audit (which check ids that are by "
                        "definition old) the only safe reading of \"missing\" is "
                        "to stop. `all_ids_resolved` is the flag to branch on: "
                        "`found:true` is true of a PARTIALLY resolved set, so a "
                        "caller reading it alone never learns two of its fifty "
                        "did not resolve. "
                        "Duplicates de-duped (first occurrence wins). "
                        "Empty array (zero elements) → falls through to "
                        "the normal list path. A comma/whitespace-joined "
                        "STRING (\"ANTS-1719,ANTS-1721\") is coerced to "
                        "this array form; a present-but-otherwise-typed or "
                        "non-empty-but-all-malformed ids refuses bad_args "
                        "rather than silently dumping the full list "
                        "(ANTS-3541). Cannot combine with `id`, `section`, "
                        "or `mode:section_index` (bad_mode_combo). Max 100 "
                        "items (bad_args). Pairs with singular `id` "
                        "(ANTS-1856).");
                    props["ids"] = idsProp;
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
                        "ANTS-4362 — an `id`/`ids` fetch returns bodies "
                        "WITHOUT this flag; a status- or section-filtered "
                        "query withholds them unless it is set, because a "
                        "filter can match the whole roadmap. A filtered reply "
                        "that withheld them carries `bodies_omitted:true` + "
                        "`bodies_omitted_reason`, so a missing body is never "
                        "mistaken for a bullet that has none. "
                        "If true, each bullet carries a `body` field "
                        "(continuation prose, truncated to ~2000 chars "
                        "with `body_truncated:true` on truncation). "
                        "ANTS-3736 — a truncated body keeps BOTH its head "
                        "and its final ~1 KiB, joined by an explicit "
                        "`… [body elided — tail follows; refetch by id "
                        "with max_body_bytes for more] …` marker, so an "
                        "append-only progress-log body still reports its "
                        "CURRENT state and not just its oldest text. "
                        "Default false. Use when triaging dense bundle "
                        "tables where the rationale lives in the body, "
                        "not the headline (ANTS-1517). ANTS-3402 — on a "
                        "TARGETED id=/ids= fetch, raise the cap with "
                        "`max_body_bytes` to read a large multi-phase epic "
                        "body in one call (list queries stay at 2000). For "
                        "a large section body, read_region section= is the "
                        "recommended untruncated path.");
                    props["include_body"] = inclBodyProp;
                    // ANTS-3402 — opt-in higher body cap for id/ids fetches.
                    QJsonObject maxBodyProp;
                    maxBodyProp["type"] = "integer";
                    maxBodyProp["description"] = QStringLiteral(
                        "Max body bytes for a TARGETED id=/ids= fetch "
                        "(clamped [2000, 16384]). ANTS-4091 — the targeted "
                        "DEFAULT is no longer 2000: it is 16384/N for an "
                        "N-id fetch, floored at 2000, so a single-id fetch "
                        "returns any body under 16 KiB whole (elision drops "
                        "the MIDDLE, where a resume plan sits) while a wide "
                        "id-set stays payload-bounded. Pass this to override. "
                        "Ignored on list / section / section_index paths, "
                        "which always emit at the 2000 cap (ANTS-3402).");
                    props["max_body_bytes"] = maxBodyProp;
                    // ANTS-3391 — `query` keyword text-filter.
                    QJsonObject queryProp;
                    queryProp["type"] = "string";
                    queryProp["description"] = QStringLiteral(
                        "Case-insensitive keyword filter (substring). "
                        "Narrows the list to bullets whose headline (or "
                        "headline_full) OR body contains this text, "
                        "composing with the status + section= filters — the "
                        "one-call \"find roadmap items mentioning X\". The "
                        "echoed `query` confirms it applied. Matches against "
                        "the same ~2000-char-capped body the list emits, so a "
                        "keyword only in a longer body's tail may be missed; "
                        "for an exact item use id / ids instead. Does NOT "
                        "combine with id / ids / mode:section_index / "
                        "mode:bundles (bad_mode_combo — those are targeted or "
                        "aggregate surfaces with no per-bullet list to "
                        "filter). ANTS-4367: a SHORT query needs narrowing — "
                        "`query:\"CI\"` matches \"decision\", \"efficiency\", "
                        "\"precision\" and \"facing\", and the damage is "
                        "REORDERING rather than padding, since genuine hits get "
                        "pushed off page one while truncated:true gives no clue "
                        "the answer is in the tail. Pass `whole_word:true` (or "
                        "`regex:true`) instead of raising `limit`, which grows "
                        "the payload at identical signal-to-noise.");
                    props["query"] = queryProp;
                    // ANTS-4367 — the two narrowing knobs.
                    QJsonObject wwProp; wwProp["type"] = "boolean";
                                        wwProp["default"] = false;
                                        wwProp["description"] = QStringLiteral(
                        "Optional (ANTS-4367). Wrap `query` in word boundaries "
                        "so a short acronym stops matching inside longer words. "
                        "Still case-insensitive — it narrows the BOUNDARY, not "
                        "the casing, so \"CI\" still finds a bullet writing it "
                        "\"ci\". A hyphenated occurrence (`CI-parity`) DOES "
                        "match, because `-` is a word boundary. Default false, "
                        "so every existing caller is byte-identical.");
                    QJsonObject qReProp; qReProp["type"] = "boolean";
                                         qReProp["default"] = false;
                                         qReProp["description"] = QStringLiteral(
                        "Optional (ANTS-4367). Treat `query` as a regular "
                        "expression (case-insensitive) — the same knob "
                        "workspace_search spells the same way, so a caller who "
                        "knows one knows the other. Wins over `whole_word` when "
                        "both are sent. A malformed pattern refuses with "
                        "bad_args rather than matching nothing silently, since "
                        "zero hits with no explanation reads as \"the roadmap "
                        "has no such items\".");
                    props["whole_word"] = wwProp;
                    props["regex"]      = qReProp;
                    // ANTS-1437 — mode arg. Default "bullets" (legacy).
                    // "section_index" returns a compact section index
                    // instead of bullets — use to discover slugs cheaply.
                    QJsonObject modeProp;
                    modeProp["type"] = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("bullets");
                    modeEnum.append("section_index");
                    // ANTS-1881 — third mode value. Narrow per-bullet
                    // shape for callers that only want the catalogue.
                    modeEnum.append("headline_only");
                    // ANTS-1922 — fourth mode value. Groups active items
                    // into thematic work-bundles for session triage.
                    modeEnum.append("bundles");
                    modeProp["enum"] = modeEnum;
                    modeProp["default"] = "bullets";
                    modeProp["description"] = QStringLiteral(
                        "Response mode. \"bullets\" (default) returns "
                        "bullets[]. \"section_index\" returns "
                        "sections[{slug, headline, level, active_count, "
                        "shipped_count, total_count, "
                        "active_count_id_only, shipped_count_id_only, "
                        "total_count_id_only}] (the `*_id_only` "
                        "parallels match the default bullets[] "
                        "predicate; ANTS-1622) and no bullets — "
                        "use for slug discovery (response < 5 KB on a "
                        "500-bullet roadmap). ANTS-1848 — honours `status`: "
                        "status:\"active\"/\"shipped\" drops sections whose "
                        "matching *_count_id_only is 0 (status:\"active\" is "
                        "the lean planning call); status:\"all\" (default) "
                        "emits every section. ANTS-1729 — section_index "
                        "now auto-truncates the sections[] array under the "
                        "~20 KB soft cap (emitting truncated/next_offset) "
                        "and accepts offset/limit to page through a "
                        "many-section roadmap. Cannot combine with "
                        "section= (bad_mode_combo). ANTS-1881 — "
                        "\"headline_only\" returns bullets[] narrowed to "
                        "{id, status, headline_oneline, section_slug} per "
                        "bullet (skips body/lanes/kind) — ~10× smaller "
                        "payload on dense bundle sections; composes with "
                        "section=, status=, id=, pagination, and ETag. "
                        "ANTS-1922 — \"bundles\" groups the active subset "
                        "(📋+🚧) into thematic work-bundles by headline-token "
                        "similarity (the one-call \"what's the next bundle "
                        "of related to-dos?\" view). Active-only — a passed "
                        "`status` is ignored; cannot combine with section=, "
                        "id, or ids (bad_mode_combo). Returns "
                        "bundles[{bundle_label, lanes, size, items[{id, "
                        "status, headline_oneline, lanes, "
                        "possibly_resolved_by?, possibly_resolved_score?, "
                        "gate_note?, blocked?}]}] sorted by size desc, plus "
                        "active_total / bundle_count / truncated. "
                        "`possibly_resolved_by` flags an item a shipped ✅ "
                        "sibling may already cover; `gate_note`/`blocked` "
                        "surface a body gate/blocker marker.");
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
                        "response would exceed ~20 KB. ANTS-3543 — on "
                        "that auto path the server first downshifts the "
                        "whole list to headline-only rows ({id, status, "
                        "headline_oneline, section_slug}) so you keep "
                        "every id instead of losing the tail, emitting "
                        "`downshifted:true`; the drop signals "
                        "(`truncated`/`next_offset`) are cleared when the "
                        "lean list is complete, and only reappear if even "
                        "the lean list overflows. Explicit limit wins "
                        "(auto-pick + downshift only fire when omitted). "
                        "When pagination applies, envelope carries "
                        "offset/limit/total/truncated and next_offset "
                        "when truncated.");
                    props["limit"] = limitProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    // ANTS-1907 — per-section ETag short-circuit (section=
                    // mode) + opt-in per-section etag emission
                    // (section_index mode). Independent of the dispatch-
                    // layer file-level etag; lets a /cold-eyes or
                    // /test-audit loop reading many sections per pass
                    // skip the re-emit on sections that didn't change.
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Per-section ETag short-circuit (ANTS-1907). "
                            "On a section= query, pass back the "
                            "`section_etag` field returned by a previous "
                            "call; when the section's bytes are unchanged "
                            "the verb returns "
                            "{ok:true, unchanged:true, section, "
                            "section_etag, path} — saves the bullets[] "
                            "body. Independent of `etag_match` (file-"
                            "level); use this when /cold-eyes / /test-"
                            "audit loops touch many sections per pass "
                            "and most are stable across an edit to one. "
                            "Only honoured in section= mode; ignored in "
                            "bullets/section_index/headline_only.");
                        props["section_etag_match"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "If true, each section in section_index "
                            "carries a `section_etag` field (ANTS-1907) "
                            "— a SHA-256 hash of the section's byte slice "
                            "stable under edits to OTHER sections. Pair "
                            "with `section_etag_match` on follow-up "
                            "section= calls to avoid re-emitting "
                            "unchanged bullets. Default false (envelope "
                            "shape unchanged when omitted; opt-in pays "
                            "a one-time per-section slicing cost).");
                        props["include_section_etags"] = p;
                    }
                    props["encoding"] = makeEncodingProp();      // ANTS-2090
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
                        "truncated_history?}. ANTS-2057 — pass "
                        "against_refs:[\"branch\", ...] to also catch a fix "
                        "that landed on the WRONG long-lived branch: a SHA "
                        "reachable from HEAD but ABSENT from a named sibling "
                        "ref is reported under mis_branched:[{bullet_id, "
                        "cited_sha, headline, missing_from:[refs]}] (plus "
                        "checked_refs, mis_branched_count, unknown_refs?, "
                        "mis_branched_truncated?). Omit against_refs for the "
                        "HEAD-only scan (envelope unchanged). SHA detector "
                        "uses an "
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
                    // ANTS-2057 — optional sibling refs for the cross-branch
                    // (mis_branched) reachability pass.
                    QJsonObject againstRefsProp;
                    againstRefsProp["type"] = "array";
                    {
                        QJsonObject items; items["type"] = "string";
                        againstRefsProp["items"] = items;
                    }
                    againstRefsProp["description"] = QStringLiteral(
                        "Optional (ANTS-2057). Sibling long-lived refs to "
                        "check cited SHAs against in addition to HEAD. A SHA "
                        "reachable from HEAD but absent from a named ref is "
                        "reported under mis_branched[] with missing_from — "
                        "catches a fix committed to the wrong branch. Refs "
                        "starting with `-`, equal to the current branch, or "
                        "that don't resolve land in unknown_refs. Capped at "
                        "10 refs.");
                    props["against_refs"] = againstRefsProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    schema["required"]   = QJsonArray{
                        QStringLiteral("caller_cwd")};
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                {
                    // ANTS-1735 — model_switch_stats: read-only effectiveness
                    // scorecard for the autonomous model switcher. Required
                    // caller_cwd; ETag + fields opt-in.
                    // ANTS-1889 — envelope carries the live switcher config
                    // (auto_model_switch_enabled / floor_tier / min_dwell_sec)
                    // and accepts an optional `scope:"global"` arg.
                    QJsonObject t;
                    t["name"] = "model_switch_stats";
                    t["description"] = QStringLiteral(
                        "Read-only effectiveness scorecard for the "
                        "autonomous model switcher (ANTS-1735): Opus turns "
                        "avoided vs regret/under-route rate — the trust "
                        "signal that auto-switching is helping. "
                        "mode: firings (default) | near_misses (per-blocker "
                        "breakdown). scope: project (default) | global. "
                        "Opt-in: etag_match, fields, compact. "
                        "caller_cwd Required.");
                    // ANTS-2079 — full envelope + headline + blocker
                    // reference in `detail` (stripped from the tools/list
                    // wire; served by tool_info {name:"model_switch_stats"}).
                    t["detail"] = QStringLiteral(
                        "Read-only scorecard for the autonomous model switcher "
                        "(ANTS-1735), scoped to the caller's project by default. "
                        "Aggregates the effectiveness ledger into Opus turns "
                        "avoided vs regret/under-route rate — the trust signal "
                        "that auto-switching is helping. Envelope: {ok, switches, "
                        "downgrades, upgrades, opus_turns_avoided, "
                        "opus_turns_routed_in, regret_count, regret_rate, "
                        "under_route_count, pending_count, by_tier, "
                        "inconclusive_count, clean_end_count, weighted_avoided, "
                        "measured_downgrades, headline_floor, "
                        "auto_model_switch_enabled, floor_tier, min_dwell_sec, "
                        "scope, headline}. ANTS-1891 — regret_count now folds "
                        "under-route harm into the numerator; measured_downgrades "
                        "excludes inconclusive 0-turn-zero-signal records "
                        "(reported as inconclusive_count instead). clean_end_count "
                        "+ weighted_avoided credit clean session-ends (no override / "
                        "correction / under-route within 10 min of session end) as "
                        "½ Opus turn avoided each, so end-of-task downgrades — the "
                        "dominant ledger shape — are no longer invisible. The "
                        "headline reads \"auto-switch OFF\" when disabled "
                        "(ANTS-2033 — the envelope then also carries "
                        "auto_model_switch_off_reason: \"never_enabled\" if the "
                        "first-run opt-in hasn't been accepted vs "
                        "\"user_disabled\" if it was shown and left off, plus a "
                        "human-readable auto_model_switch_off_detail; the switch "
                        "is a single global key, so there is no per-project "
                        "opt-out state), "
                        "\"auto-switch ON (floor=X, dwell=Ns) … no switches "
                        "yet\" when enabled with no records, \"calibrating "
                        "(N/F measured)\" (post-1909 — was \"insufficient "
                        "data\" pre-1909) until the headline floor (F=10) is "
                        "reached, and the full avoided/clean-end/regret "
                        "breakdown above the floor — so a caller can "
                        "distinguish \"feature dormant\" from \"feature "
                        "working quietly\" from \"feature "
                        "still in calibration.\" An absent ledger returns "
                        "{ok:true, switches:0, …}; pending records (switch near "
                        "session end, outcome not yet measured) are counted "
                        "separately, never as success. Envelope readers should "
                        "check measured_downgrades > 0 before treating "
                        "regret_rate as meaningful — the new inconclusive_count "
                        "makes the gap visible. Pass scope:\"global\" to "
                        "aggregate across all projects in the ledger instead of "
                        "filtering to the caller's project (ANTS-1889). "
                        "ANTS-1894 — envelope additionally carries a slim "
                        "`near_misses:{total_24h, dominant_blocker}` block "
                        "summarising auto-switch decisions that were "
                        "evaluated-but-blocked (composer_not_empty, "
                        "dwell_time_insufficient, override_cooldown_active, "
                        "etc.) — diagnostic for \"why doesn't it switch in "
                        "this project?\". Pass `mode:\"near_misses\"` for "
                        "the full per-blocker breakdown (24 h + all-time "
                        "windows, distinct_signatures count). ANTS-1909 — "
                        "the headline now carries the dwell parenthetical "
                        "(`floor=X, dwell=Ns`), uses \"calibrating\" in "
                        "place of \"insufficient data\" below the floor, "
                        "and — when the 24 h near-miss block is non-empty — "
                        "appends \"N near-misses in 24 h blocked by "
                        "<dominant_blocker>\" on both the no-switches and "
                        "calibrating branches so the trust signal reads "
                        "as \"evaluating but blocked\" rather than "
                        "\"feature did nothing\". Blocker-meanings (for "
                        "the dominant-blocker token): "
                        "`composer_not_empty` = focused-tab composer "
                        "carries text (hard veto; ANTS-1908 tracks a "
                        "stale-text soft-veto); "
                        "`dwell_time_insufficient` = haven't reached "
                        "min_dwell_sec on the current tier yet (transient, "
                        "self-resolving); "
                        "`override_cooldown_active` = user manually picked "
                        "the current tier within the cool-down window "
                        "(transient); "
                        "`target_equals_current` = recommender already on "
                        "the target tier — *expected steady-state*, NOT a "
                        "failure; "
                        "`ticks_target_stable_insufficient` = recommender "
                        "hasn't held the new target long enough to act on — "
                        "*expected steady-state*, NOT a failure. "
                        "ANTS-1949 — `by_trigger.manual` is structurally 0: "
                        "every ledger record carries trigger=\\\"auto\\\" because "
                        "user /model commands and chip-clicks are NOT appended "
                        "to the firing ledger (only the autonomous switcher "
                        "writes records). Implication: `opus_turns_routed_in` "
                        "counts turns on Opus after an autonomous upgrade only; "
                        "it cannot see human-forced Opus sessions.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to check whether automatic model switching is "
                        "paying off before trusting it more widely — reports "
                        "avoided Opus turns vs regret rate. scope:\"global\" "
                        "aggregates across all projects.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    props["fields"]     = makeFieldsProp();
                    props["compact"]    = makeCompactProp();     // ANTS-2091
                    {
                        QJsonObject scopeProp;
                        scopeProp["type"]        = QStringLiteral("string");
                        scopeProp["enum"]        = QJsonArray{
                            QStringLiteral("project"),
                            QStringLiteral("global")};
                        scopeProp["description"] = QStringLiteral(
                            "Optional aggregation scope (ANTS-1889). "
                            "\"project\" (default) filters the ledger to the "
                            "caller's project root; \"global\" aggregates "
                            "across all projects in the ledger.");
                        props["scope"] = scopeProp;
                    }
                    {
                        // ANTS-1894 — mode arm. Optional; defaults to "firings".
                        // Per mcp-tools.md § 10 schema-hygiene: not in required[];
                        // per-property description set.
                        QJsonObject modeProp;
                        modeProp["type"]        = QStringLiteral("string");
                        modeProp["enum"]        = QJsonArray{
                            QStringLiteral("firings"),
                            QStringLiteral("near_misses")};
                        modeProp["description"] = QStringLiteral(
                            "Optional mode arm (ANTS-1894). \"firings\" "
                            "(default) returns the firing envelope with a "
                            "slim near_misses block; \"near_misses\" returns "
                            "the full near-miss breakdown (24 h + all-time "
                            "windows, by_blocked_by counts, dominant_blocker, "
                            "distinct_signatures). Note: by_blocked_by values "
                            "can sum to MORE than window_24h.total because a "
                            "single near-miss may be blocked by several gates "
                            "simultaneously (ANTS-1947: total counts events, "
                            "by_blocked_by counts gate-hits). ANTS-1972 — the "
                            "firings-mode slim `near_misses.total_24h` EXCLUDES "
                            "idle-end-suppressed near-misses while near_misses-"
                            "mode `window_24h.total` INCLUDES them; at the same "
                            "instant firings.total_24h + "
                            "firings.idle_end_suppressed_24h == "
                            "near_misses-mode window_24h.total (not a bug — the "
                            "slim block reports actionable gaps only).");
                        props["mode"] = modeProp;
                    }
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
                    "claude_running, color}. When a tab runs a tracked "
                    "Claude session it also carries (ANTS-1865) "
                    "`claude_state` (not_running|idle|thinking|tool_use|"
                    "compacting), `awaiting_input` (bool — permission prompt "
                    "active), and the lean overlays `plan_mode` / `auditing` "
                    "(emitted only when true) + `tool` (the active tool name "
                    "when tool_use) — so a session can verify the per-tab "
                    "Claude dot/prompt state programmatically instead of "
                    "eyeballing the tab strip. Envelope: {ok:true, tabs:[…]}.");
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
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
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

                // ANTS-1301 — recent_errors: scan recent scrollback for
                // structured compile/test/runtime errors.
                {
                    QJsonObject t;
                    t["name"] = "recent_errors";
                    t["description"] = QStringLiteral(
                        "Scan the most recent N lines of a terminal's "
                        "scrollback for structured errors — GCC/clang "
                        "`error:`, ruff/flake8 `file:line:col: CODE`, "
                        "`lua: file:line:`, ctest `(Failed)`/`***Failed`, "
                        "and Python tracebacks. Returns {ok, errors:["
                        "{category, file?, line?, column?, message, "
                        "text}], errors_count, lines_scanned, "
                        "truncated}. `category` is compiler/lint/lua/"
                        "test/python. Use instead of re-running the "
                        "command or `get_text`-ing the whole buffer to "
                        "answer \"what just went wrong in this "
                        "terminal?\". Optional `tab` (explicit index), "
                        "`caller_cwd` (anchors to your tab when `tab` "
                        "is omitted), `lines` (default 500, cap 10000), "
                        "`max_results` (default 50, cap 1000 — keeps the "
                        "newest when capped). Refuses `no_window` when "
                        "no terminal resolves.");
                    t["selection_hint"] = QStringLiteral(
                        "Use right after a build/test/lint command "
                        "fails — far cheaper than get_text + eyeballing. "
                        "For the structured ctest summary of a recorded "
                        "run, prefer test_results.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Explicit tab index. Omit to use your own "
                            "tab (via caller_cwd) or the focused tab.");
                        props["tab"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["default"] = 500;
                        p["description"] = QStringLiteral(
                            "Trailing scrollback lines to scan "
                            "(default 500, cap 10000).");
                        props["lines"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on errors[] (default 50, cap 1000); "
                            "the newest are kept when capped. "
                            "errors_count carries the pre-cap total.");
                        props["max_results"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1312 — last_selection: return the focused (or
                // routed) terminal's current selection text.
                {
                    QJsonObject t;
                    t["name"] = "last_selection";
                    t["description"] = QStringLiteral(
                        "Return the focused (or routed) terminal's "
                        "current selection text — the text the user "
                        "highlighted with the mouse. Use this instead "
                        "of walking the scrollback to find the error / "
                        "stack trace / config snippet the user is "
                        "pointing at. Saves the equivalent of a "
                        "`get_text lines=500` round-trip whenever the "
                        "context Claude needs is the same text the "
                        "user just selected. Returns {ok, "
                        "has_selection, text, length, bytes}. When the "
                        "user has no active selection, `has_selection` "
                        "is false and `text` is the empty string. "
                        "Optional `tab` (explicit index), `caller_cwd` "
                        "(anchors to your tab when `tab` is omitted). "
                        "Refuses `no_window` when no terminal "
                        "resolves.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when the user says \"this\" / \"that "
                        "error\" / \"the line I selected\" — far "
                        "cheaper than scanning scrollback. For a "
                        "structured error block from a recent build, "
                        "prefer recent_errors.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Explicit tab index. Omit to use your own "
                            "tab (via caller_cwd) or the focused tab.");
                        props["tab"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1636 — find_sources: topic-to-files discovery.
                {
                    QJsonObject t;
                    t["name"] = "find_sources";
                    t["description"] = QStringLiteral(
                        "Map a free-text topic (\"audit cache "
                        "invalidation\", \"test audit fold-in\", "
                        "\"model auto switch actuator\") to a ranked "
                        "list of source files under the project's "
                        "src/ + tests/ trees. Use this when you don't "
                        "already know the symbol/filename and would "
                        "otherwise run 3-4 grep/find cycles. Returns "
                        "{ok, files:[{path, score, role, evidence}], "
                        "files_count, unmatched_terms, files_scanned, "
                        "truncated}. `role` ∈ impl|header|test; "
                        "`evidence` carries up to 3 short \"matched "
                        "filename …\" / \"\\\"…\\\" × N in body\" "
                        "explanations. Required: `topic`, `caller_cwd`. "
                        "Optional `max_results` (default 20, server "
                        "hard-cap 100). Compared to workspace_search: "
                        "this tool expects a topic, not an exact "
                        "symbol — it auto-expands snake/camel/dropped-"
                        "separator variants and ranks files (not "
                        "lines). Token-savings: typically replaces a "
                        "3-4 round-trip grep + read cycle with one "
                        "MCP call.");
                    // ANTS-3619 — lead with the language limit. The
                    // envelope already explains it on a zero-result, but
                    // that is after the fact: files_count:0 on a Python
                    // project is indistinguishable from a genuine
                    // "nothing calls this", which is the worst possible
                    // wrong answer when checking a change's blast radius.
                    // Must start with "Use " and stay under 240 chars
                    // (ANTS-1897 INV-7 / ANTS-1453 HINT-3) — the
                    // qualifier has to fit that budget, so it leads and
                    // the "prefer find_definition for known symbols"
                    // advice moves to the description above.
                    t["selection_hint"] = QStringLiteral(
                        "Use for topic discovery in C/C++ ONLY — on "
                        "Python/JS/Go it returns files_count:0, which "
                        "reads as \"no callers\" but is not an answer; "
                        "use codebase_index or workspace_search there. "
                        "find_definition IS multi-language — do not copy "
                        "its reach.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Free-text topic. Tokens shorter than 3 "
                            "chars and roadmap IDs (ANTS-NNNN) are "
                            "ignored.");
                        props["topic"] = p;
                    }
                    {
                        // ANTS-3415 — `symbol` alias for `topic`. A session
                        // reaching for the "who defines/uses X" verb by
                        // symbol name (natural from the catalog one-liner)
                        // was refused bad_args; accept it as an alias so the
                        // call succeeds. `topic` wins when both are set. For
                        // call-sites of a known symbol, prefer find_caller.
                        QJsonObject p; p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Alias for `topic` (ANTS-3415). `topic` takes "
                            "precedence when both are present. To list the "
                            "call-sites of a known symbol, prefer find_caller.");
                        props["symbol"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["default"] = 20;
                        p["description"] = QStringLiteral(
                            "Cap on files[] (default 20, hard-cap 100). "
                            "`files_count` carries the post-cap count; "
                            "`truncated` is true when the pre-cap list "
                            "was longer.");
                        props["max_results"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["encoding"] = makeEncodingProp();      // ANTS-2090
                    schema["properties"] = props;
                    QJsonArray req; req.append(QStringLiteral("topic"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-3368 — co_change_family: the grouped edit-site
                // checklist for one settings-backed field.
                {
                    QJsonObject t;
                    t["name"] = "co_change_family";
                    t["description"] = QStringLiteral(
                        "Given one exemplar settings field, list every edit "
                        "site you must touch to mirror it — grouped by file, "
                        "with the JSON string key and the derived names "
                        "(setX, m_X, XChanged) a whole-word symbol search "
                        "misses. Matches on the longest run of the stem's "
                        "words that is contiguous in both names, so "
                        "setMcpEnabled still belongs to claudeMcpEnabled. "
                        "Returns {ok, stems, stem_words, min_run, "
                        "files:[{path, sites:[{line, stem, name, role, run, "
                        "run_len, text}]}], files_count, sites_count, "
                        "truncated}. `role` is LEXICAL, one of "
                        "json_key|member|mutator|signal|type|reference. "
                        "Required: `caller_cwd` plus `stem` or `stems`. "
                        "Refusals: bad_args, no_project, rg_failed. "
                        "Scans the whole repo (minus .gitignore), unlike "
                        "find_sources.");
                    t["detail"] = QStringLiteral(
                        "min_run is the shortest accepted word-run, resolved "
                        "per stem against that stem's own word count: it "
                        "defaults to min(2, words) and clamps to 1..words. "
                        "Setting it to 1 widens the SCAN, not just the "
                        "filter — the default pattern alternates adjacent "
                        "word pairs and can never produce a one-word match, "
                        "so min_run:1 is what reaches a site sharing a "
                        "single word (audioLod from lodEnabled). That is "
                        "much more expensive on a common word. A run of "
                        "nothing but stopwords (get/set/is/has/on/off/"
                        "enabled/disabled/value/data/flag/count/size/index/"
                        "name/type/mode/m/p) is dropped, and a stem made "
                        "only of stopwords refuses bad_args rather than "
                        "returning a silent empty result. One row per "
                        "(path, line): a line matching several stems is "
                        "emitted once, owned by the longest run, ties by "
                        "position in stems. Files are ordered by their "
                        "maximum run_len descending then path; sites by "
                        "line. When max_sites binds, the sites retained are "
                        "those with the highest run_len — never the first N "
                        "in scan order — and truncated is set. truncated is "
                        "also set when the ripgrep budget is exhausted; "
                        "rg_failed is reserved for a scanner that did not "
                        "run at all. Contract: docs/specs/"
                        "ANTS-3368-co-change-family.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when mirroring an existing settings field and "
                        "you need every file to touch. find_sources returns "
                        "ranked files under src/+tests/; this returns lines "
                        "repo-wide, including the JSON key and docs.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p; p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "One exemplar name — an identifier "
                            "(claudeMcpEnabled) or a config key "
                            "(claude.mcp_enabled). Sugar for a one-element "
                            "`stems`; `stems` wins when both are sent. Must "
                            "match ^[A-Za-z0-9_.-]+$.");
                        props["stem"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "array";
                        QJsonObject items; items["type"] = "string";
                        p["items"] = items;
                        p["description"] = QStringLiteral(
                            "The field group; the union of each stem's "
                            "family. Each site carries the stem that owns "
                            "it. Required unless `stem` is given.");
                        props["stems"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["description"] = QStringLiteral(
                            "Shortest accepted word-run (default "
                            "min(2, stem words), clamped per stem to "
                            "1..words, never refused). 1 widens the scan "
                            "itself — see tool_info detail.");
                        props["min_run"] = p;
                    }
                    {
                        QJsonObject p; p["type"] = "integer";
                        p["default"] = 200;
                        p["description"] = QStringLiteral(
                            "Cap on sites (default 200, clamped to "
                            "1..1000). When it binds, the highest-run_len "
                            "sites are kept and `truncated` is true.");
                        props["max_sites"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    props["fields"]     = makeFieldsProp();
                    // No `encoding` prop: the columnar repack is for a
                    // top-level array of FLAT objects, and files[] carries a
                    // nested sites[] per row. ANTS-3368 § 2.4 does not
                    // declare it either.
                    schema["properties"] = props;
                    // `stem`/`stems` is an either-or that required[] cannot
                    // express, so it is a runtime check refusing bad_args.
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1248: workspace_search — ripgrep wrapper. The
                // schema declares all 7 spec args and marks `pattern`
                // as required. The description names the alternative
                // bash idiom + token-saving headline so Claude prefers
                // this over Bash/Read for code searches.
                QJsonObject wsTool;
                wsTool["name"] = "workspace_search";
                wsTool["description"] = QStringLiteral(
                    "Search the project for code matching a literal string "
                    "or regex — prefer over `Bash grep -r` (saves "
                    "250-4500 tokens). Returns {ok, matches:[{file, "
                    "line, text, also_at?, context_*?}], truncated, dedup, "
                    "...}. Args: pattern (required; alias query), regex, "
                    "lane, glob, max_results (cap 500), context [0,10], "
                    "case, respect_gitignore, include_hidden, dedup, "
                    "timeout_sec [1,30], max_match_bytes, headline_only, "
                    "enclosing_symbol. "
                    "caller_cwd anchors the project root (or '~global' for "
                    "~/.claude/). The query is ONE literal/regex pattern, not "
                    "AND-combined words — a multi-word query that hits 0 "
                    "matches returns an advisory `hint` (pass a single token, "
                    "or regex:true with .* between terms). Hard-kill returns "
                    "rg_failed with a hint.");
                // ANTS-2079 — full per-arg reference in `detail` (stripped
                // from the tools/list wire; served by tool_info
                // {name:"workspace_search"}).
                wsTool["detail"] = QStringLiteral(
                    "Search the project for code matching a literal "
                    "string or regex. Returns {ok, matches:[{file, "
                    "line, text, also_at?:[{file,line}…], "
                    "context_before?:[{line,text}], "
                    "context_after?:[{line,text}]}], truncated, "
                    "dedup, dedup_collapsed, respect_gitignore, "
                    "include_hidden, timeout_sec, elapsed_ms}. Prefer "
                    "this over `Bash grep -r ...` — typically saves "
                    "250-4500 tokens per query and avoids round-trips "
                    "for no-match cases. Args: pattern (required; "
                    "alias `query` — ANTS-2041), "
                    "regex (false), lane (subdir under project root), "
                    "glob, max_results (default 50, cap 500), context "
                    "(default 0, server-clamped to [0,10] — when > 0, "
                    "each match carries `context_before`/`context_after` "
                    "arrays with up to N surrounding lines per side; "
                    "ANTS-1304), case (smart/sensitive/insensitive), "
                    "respect_gitignore (default true — pass false for "
                    "stale-path audits across build outputs / "
                    "compile_commands.json / cache dirs), "
                    "include_hidden (default false — pass true to "
                    "search dotfile paths; .git/ stays excluded "
                    "regardless), dedup (default true — collapses "
                    "near-duplicate excerpts into a single primary "
                    "match with `also_at` carrying the rest; "
                    "context is preserved on the primary only, pass "
                    "dedup:false for per-hit context; ANTS-1501), "
                    "timeout_sec (default 5, range [1,30] "
                    "— raise for mid-size projects > 2 k files; "
                    "ANTS-1565). ANTS-1876: opt-in payload knobs — "
                    "`max_match_bytes` (clip every `text` / "
                    "`headline` to N UTF-8 bytes, default off, range "
                    "[50, 10000]) and `headline_only:true` (emit "
                    "`{file, line, headline}` triples without "
                    "`text` / `context_*` — pair with "
                    "`max_match_bytes` for ~10× wire reduction on "
                    "dense bundle sweeps). ANTS-2220: "
                    "`enclosing_symbol:true` annotates each match with "
                    "`enclosing:\"Foo::bar\"` (the function/method it "
                    "lives inside, via the file outline's "
                    "nearest-preceding symbol) — folds the post-search "
                    "\"which function?\" lookup into the search; costs "
                    "one outline scan per distinct matched file, so it "
                    "is off by default. ANTS-3537: `count_only:true` "
                    "returns {count, files_count, truncated} with "
                    "matches[] omitted — a rows-eliminated "
                    "existence/frequency mode; `count` is the true "
                    "total (uncapped by max_results). On hard-kill the "
                    "rg_failed envelope carries a `hint` field with "
                    "the three viable next steps. ANTS-3549: "
                    "`files_only:true` returns {files:[{file,count}], "
                    "files_count} with no match rows — the distinct "
                    "matched-file set for \"which files reference X?\". "
                    "ANTS-3547: `offset` pages a truncated search — the reply "
                    "carries `next_offset`; pass it back as `offset` to "
                    "continue instead of re-running wider. ANTS-1390: pass "
                    "`caller_cwd: "
                    "\"~global\"` (alias `\"~claude-config\"`) to "
                    "search ~/.claude/ instead of a project root — "
                    "for sessions editing global Claude config "
                    "(skills, agents, the global CLAUDE.md).");
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
                        QStringLiteral("Ripgrep --glob filter (e.g. \"*.cpp\"). "
                                       "INCLUSION only — a leading \"!\" refuses "
                                       "bad_glob; use exclude_glob instead.");
                    // ANTS-3704 — the "everywhere EXCEPT here" half. Separate
                    // arg rather than a "!" in `glob`, so `glob` stays
                    // unambiguously positive at the call site.
                    QJsonObject exGlobProp;
                    { QJsonArray ty; ty.append(QStringLiteral("string"));
                                     ty.append(QStringLiteral("array"));
                      exGlobProp["type"] = ty; }
                    exGlobProp["description"] = QStringLiteral(
                        "Path glob(s) to EXCLUDE — a string or an array, each "
                        "rendered as ripgrep --glob '!<pattern>' after `glob` so "
                        "last-one-wins narrows rather than widens. Write the "
                        "pattern positively (\"docs/**\", not \"!docs/**\"). Use for "
                        "\"search everywhere but the prose\" on a doc-heavy repo, "
                        "which `lane` (one subdir) and `glob` (one positive "
                        "pattern) cannot express.");
                    QJsonObject maxProp;      maxProp["type"]      = "integer";
                                              maxProp["default"]   = 50;
                                              maxProp["maximum"]   = 500;
                    // ANTS-1304: context surfaces ±N surrounding
                    // lines on each match (context_before / context_after).
                    // Server clamp [0, 10] applied in cmdWorkspaceSearch.
                    QJsonObject ctxProp;      ctxProp["type"]      = "integer";
                                              ctxProp["default"]   = 0;
                                              ctxProp["minimum"]   = 0;
                                              ctxProp["maximum"]   = 10;
                                              ctxProp["description"] = QStringLiteral(
                        "Surrounding-lines window. When > 0 (server-"
                        "clamped to 10), each match carries "
                        "`context_before` and `context_after` arrays "
                        "of up to N `{line, text}` entries. When 0 "
                        "(default), the compact pre-1304 envelope "
                        "ships.");
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
                    // ANTS-1293 — response byte cap. max_results bounds
                    // the count; this bounds total size. Trims matches[]
                    // from the tail and sets truncated + results_dropped.
                    QJsonObject maxBytesProp;
                    maxBytesProp["type"]    = "integer";
                    maxBytesProp["minimum"] = 1;
                    maxBytesProp["description"] = QStringLiteral(
                        "Cap on the serialized response in bytes "
                        "(default 512 KiB, server-clamped to 4 MiB). When "
                        "exceeded, trailing matches are dropped and the "
                        "envelope carries truncated:true + "
                        "results_dropped:<n> (+ bytes_cap_clamped:true if "
                        "the requested cap exceeded the ceiling). "
                        "ANTS-3543 — unless you passed mode=headline_only, "
                        "the server first downshifts the whole match set "
                        "to lean {file,line,headline} rows and re-caps, "
                        "emitting `downshifted:true` + `headline_only:true` "
                        "(so headline_only may appear on a response you "
                        "did not request) and clearing the drop signals "
                        "when the lean set is complete.");
                    // ANTS-1876 — per-match text clip. Out-of-range
                    // falls back to default (off, 0); clamp range
                    // [50, 10000] guarantees at least the ellipsis +
                    // 47 bytes of payload.
                    QJsonObject mmbProp;
                    mmbProp["type"]    = "integer";
                    // ANTS-3548 — default-ON clip (512). `minimum` is 0
                    // (not 50) so the `0` opt-out sentinel is in-range /
                    // passable; the effective CLIP range is still
                    // [50, 10000] (server clamps 1..49 up to 50).
                    mmbProp["default"] = 512;
                    mmbProp["minimum"] = 0;
                    mmbProp["maximum"] = 10000;
                    mmbProp["description"] = QStringLiteral(
                        "Per-match `text` (or `headline`, if "
                        "`headline_only:true`) clip in UTF-8 bytes. "
                        "Every text-bearing field — primary `text`, "
                        "every `text` inside `context_before` / "
                        "`context_after` — is clipped to *exactly* this "
                        "many UTF-8 bytes (payload prefix + 3-byte "
                        "ellipsis \"\xE2\x80\xA6\"). Fields whose unclipped "
                        "form already fits are emitted verbatim. "
                        "ANTS-3548: **default-ON** — absent → 512 (a "
                        "token-saver clip on long/pathological lines); "
                        "pass **0 to opt out** (no clip). An explicit "
                        "value clamps to the [50, 10000] clip range. "
                        "Dedup runs BEFORE the clip so the key is "
                        "unaffected (ANTS-1876 INV-4). The effective clip "
                        "value echoes on the envelope (so a default call "
                        "carries `max_match_bytes:512`).");
                    // ANTS-1876 — headline_only summary shape:
                    // emit {file, line, headline} triples instead of
                    // the bullets-mode {file, line, text, context_*}
                    // shape. Drops context entirely; rename text →
                    // headline. Composes with `max_match_bytes` (the
                    // headline field gets clipped just like text
                    // would have).
                    QJsonObject hoProp;
                    hoProp["type"]    = "boolean";
                    hoProp["default"] = false;
                    hoProp["description"] = QStringLiteral(
                        "When true, each match emits as "
                        "`{file, line, headline}` (where `headline` "
                        "is the matched line, renamed from `text`) "
                        "plus optional `also_at` (unchanged). "
                        "`context_before` / `context_after` are "
                        "dropped even when `context > 0` was passed. "
                        "`also_at` entries remain `{file, line}` "
                        "(no text to clip). Pairs with "
                        "`max_match_bytes` for ~10× wire reduction "
                        "on dense bundle sweeps (ANTS-1876).");
                    // ANTS-2041 — `query` alias for `pattern`. Declared
                    // so the schema advertises it; the handler reads it
                    // only when `pattern` is absent. `pattern` stays the
                    // canonical (required) arg.
                    QJsonObject queryProp;  queryProp["type"] = "string";
                    queryProp["description"] = QStringLiteral(
                        "Alias for `pattern` — used only when `pattern` "
                        "is absent/empty. Prefer `pattern` (the "
                        "canonical, required arg).");
                    // ANTS-2220 — enclosing_symbol: annotate each match
                    // with the function/method it lives inside.
                    QJsonObject encProp;  encProp["type"] = "boolean";
                    encProp["default"] = false;
                    encProp["description"] = QStringLiteral(
                        "When true, annotate each match with "
                        "`enclosing:\"Foo::bar\"` — the function/method it "
                        "lives inside (nearest-preceding symbol from the "
                        "file outline) — folding the usual \"which function "
                        "is this in?\" follow-up into the search. Costs one "
                        "file-outline scan per distinct matched file, so it "
                        "is off by default. A match before the first symbol "
                        "(e.g. in includes) carries no `enclosing`.");
                    // ANTS-3537 — count_only: rows-eliminated existence /
                    // frequency mode. Omits matches[] entirely; returns
                    // just the totals.
                    QJsonObject countOnlyProp;
                    countOnlyProp["type"] = "boolean";
                    countOnlyProp["default"] = false;
                    countOnlyProp["description"] = QStringLiteral(
                        "When true, run the search but return only "
                        "`{count, files_count, truncated, "
                        "count_only:true}` — matches[] is omitted "
                        "entirely (the rg scan still runs; the rows are "
                        "never serialised). A rows-ELIMINATED mode for "
                        "existence / frequency checks (\"is X referenced "
                        "anywhere?\", \"how many call-sites?\") that would "
                        "otherwise pay for row bodies they discard. "
                        "`count` is the TRUE total match count (uncapped "
                        "by max_results); `files_count` is the number of "
                        "files with a match. `truncated` is true only "
                        "when the scan was cut off (hard-kill / parse "
                        "budget), never merely because the row cap was "
                        "hit. Complements headline_only (row-shape trim) "
                        "and max_match_bytes (row-length trim).");
                    // ANTS-3549 — files_only: rows-eliminated "which files
                    // matched" mode. Distinct matched-file set + per-file
                    // counts; drops the match rows.
                    QJsonObject filesOnlyProp;
                    filesOnlyProp["type"] = "boolean";
                    filesOnlyProp["default"] = false;
                    filesOnlyProp["description"] = QStringLiteral(
                        "When true, return only `{files:[{file, count}], "
                        "files_count, count, truncated, files_only:true}` — "
                        "the DISTINCT set of files that matched, each with its "
                        "hit count, and NO match rows. A rows-ELIMINATED mode "
                        "for \"which files reference X?\" when you will open "
                        "the files next anyway; much smaller than "
                        "headline_only when a symbol recurs many times in one "
                        "file. `count` is the true total match count across "
                        "all files (uncapped by max_results); the file list "
                        "is complete (not capped by max_results). `truncated` "
                        "is true only when the scan was cut off (hard-kill / "
                        "parse budget). count_only (leaner still) takes "
                        "precedence if both are set. Complements count_only "
                        "(counts only) and headline_only (one line per "
                        "match).");
                    // ANTS-3547 — offset cursor: continue a truncated search
                    // instead of re-running it wider. (Named wsOffsetProp so
                    // the source-grep test can scope it to workspace_search.)
                    QJsonObject wsOffsetProp;
                    wsOffsetProp["type"] = "integer";
                    wsOffsetProp["default"] = 0;
                    wsOffsetProp["minimum"] = 0;
                    wsOffsetProp["description"] = QStringLiteral(
                        "Skip the first N matches, returning the window "
                        "[offset, offset+max_results). Use to CONTINUE a "
                        "truncated search (paging) instead of re-running it "
                        "wider — which re-scans from scratch and re-emits "
                        "every match already seen. When more matches remain, "
                        "the response carries `next_offset`; pass it back as "
                        "`offset` for the next page (mirrors roadmap_query). "
                        "Pairs with count_only (ANTS-3537): count first, then "
                        "page. Dedup applies within each page. Default 0.");
                    props["pattern"]     = patternProp;
                    props["enclosing_symbol"] = encProp;          // ANTS-2220
                    props["query"]       = queryProp;
                    props["regex"]       = regexProp;
                    props["lane"]        = laneProp;
                    props["glob"]        = globProp;
                    props["exclude_glob"] = exGlobProp;  // ANTS-3704
                    props["max_results"] = maxProp;
                    props["max_bytes"]   = maxBytesProp;
                    props["max_match_bytes"] = mmbProp;       // ANTS-1876
                    props["headline_only"]   = hoProp;        // ANTS-1876
                    props["count_only"]      = countOnlyProp; // ANTS-3537
                    props["files_only"]      = filesOnlyProp; // ANTS-3549
                    // ANTS-4388 — distinct-MATCH mode.
                    {
                        QJsonObject mo; mo["type"] = "boolean";
                                        mo["default"] = false;
                                        mo["description"] = QStringLiteral(
                            "Optional (ANTS-4388). Return the DISTINCT MATCHED "
                            "SUBSTRINGS instead of rows: {matches:[{text, "
                            "count, files_count}], distinct_count}. Every other "
                            "trim here is row-shaped (count_only drops rows, "
                            "files_only gives one row per file, headline_only "
                            "thins rows, max_match_bytes shortens rows), so "
                            "\"what is the SET of X in this tree\" had no "
                            "answer. `dedup` is NOT this — its key is the "
                            "whitespace-normalised LINE, so two lines carrying "
                            "the same token stay two rows and one line carrying "
                            "two tokens stays one. Nor is it count_only, which "
                            "counts occurrences of ONE pattern; this asks which "
                            "distinct strings that pattern matched. The cap "
                            "applies to DISTINCT VALUES, not occurrences, so a "
                            "3-string answer is never truncated by a 50-row "
                            "limit; `count` / `files_count` come from the full "
                            "uncapped scan. Best with regex:true. count_only "
                            "wins if both are set.");
                        props["matches_only"] = mo;
                    }
                    props["offset"]      = wsOffsetProp;      // ANTS-3547
                    props["context"]     = ctxProp;
                    props["case"]        = caseProp;
                    props["respect_gitignore"] = respectGitignoreProp;
                    props["include_hidden"]    = includeHiddenProp;
                    props["dedup"]             = dedupProp;
                    props["timeout_sec"]       = timeoutSecProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["raw"]         = makeRawProp();        // ANTS-2218
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    props["encoding"]    = makeEncodingProp();   // ANTS-2090
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("pattern");
                    schema["required"] = required;
                    wsTool["inputSchema"] = schema;
                }
                tools.append(wsTool);

                // ANTS-3716 — cited_by: which document cites which anchor, in
                // one call. Spec: docs/specs/ANTS-3716-cited-by-sweep.md.
                QJsonObject cbTool;
                cbTool["name"] = "cited_by";
                cbTool["description"] = QStringLiteral(
                    "Given the anchors a change touched (symbols, flags, config "
                    "keys, paths), report which documents cite which of them — one "
                    "call in place of one search per anchor. Returns {ok, "
                    "cells:[{anchor, file, count, first_line}], cells_count, "
                    "anchors_matched, anchors_unmatched, files_count, "
                    "scope_resolved, truncated}. `count` is OCCURRENCES, not "
                    "matching lines. Anchors match LITERALLY — for a regex use "
                    "workspace_search. `anchors_unmatched` is a first-class "
                    "result: \"nothing cites this\" is the cheap half of a sweep. "
                    "`scope` defaults to the project's docs dir + README.md + "
                    "CLAUDE.md; an entry absent on disk is pruned and drops out of "
                    "`scope_resolved`. Refusals: bad_args (anchors absent, empty, "
                    "over 64, an empty-string anchor, or case \"smart\"), bad_path "
                    "(scope escapes the project root), rg_failed. caller_cwd "
                    "Required. Full detail via tool_info {name:\"cited_by\"}.");
                cbTool["detail"] = QStringLiteral(
                    "One rg run per anchor, in sorted anchor order, so every match "
                    "belongs to its anchor by construction and there is no "
                    "attribution step. A combined single pass was measured and "
                    "rejected: rg reports the matched TEXT rather than the pattern "
                    "(so a case-insensitive hit cannot be mapped back to its "
                    "anchor), and with several -F patterns a nested anchor loses "
                    "leftmost-first and is reported uncited. 64 sequential runs "
                    "over a docs tree cost ~0.45 s against a 5 s default budget, "
                    "and it is still one round-trip either way.\n"
                    "`case` is \"insensitive\" by default and there is NO \"smart\" "
                    "mode: rg resolves --smart-case over the combined pattern set, "
                    "so one anchor carrying a capital would silently flip every "
                    "other anchor in the request to case-sensitive.\n"
                    "`max_cells` (default 500, clamp 1-5000) caps cells[] AFTER "
                    "the (anchor, file) sort, so two calls over an unchanged tree "
                    "return byte-identical bodies. `cells_count` is the capped "
                    "length; `files_count` is the UNCAPPED number of distinct "
                    "matching files. anchors_matched / anchors_unmatched are "
                    "computed over every run and stay meaningful when truncated "
                    "is true. A 50,000-cell collection ceiling guards the "
                    "pathological case; past it collection stops but every run "
                    "still drains, so no anchor is misreported as uncited.\n"
                    "`timeout_sec` (default 5, clamp 1-30) is the budget for the "
                    "WHOLE anchor set, not per run. Any failed rg run refuses "
                    "rg_failed and discards the cells already tallied — a partial "
                    "cell set is indistinguishable from a complete one.\n"
                    "Overlapping scope entries are de-overlapped before the run "
                    "(rg searches a file once per positional path that reaches it, "
                    "which would double a cell's count). No line bodies: "
                    "first_line is enough to get there, and read_region is the "
                    "drill-in. ETag-304 supported.");
                cbTool["selection_hint"] = QStringLiteral(
                    "Use after a change to ask \"which documents mention any of "
                    "these?\" — one call instead of one workspace_search per "
                    "anchor, and it names the anchors nothing cites. Not a "
                    "replacement for workspace_search: no regexes, no line "
                    "bodies.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject items;
                        items["type"] = "string";
                        p["items"] = items;
                        p["description"] = QStringLiteral(
                            "The changed names, 1-64 literal strings — symbol, "
                            "flag, config key, env var or path. Never regexes. An "
                            "empty array, over 64 entries, or an empty-string "
                            "element refuses bad_args.");
                        props["anchors"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject items;
                        items["type"] = "string";
                        p["items"] = items;
                        p["description"] = QStringLiteral(
                            "Project-relative files and/or dirs to search. "
                            "Defaults to the project's docs dir, README.md and "
                            "CLAUDE.md. Entries absent on disk are pruned; "
                            "scope_resolved echoes what was actually searched.");
                        props["scope"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append("insensitive");
                        e.append("sensitive");
                        p["enum"]    = e;
                        p["default"] = "insensitive";
                        p["description"] = QStringLiteral(
                            "Match case. \"smart\" is deliberately absent — rg "
                            "resolves it over the whole pattern set, so one anchor "
                            "would change every other anchor's result.");
                        props["case"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["default"]     = 500;
                        p["minimum"]     = 1;
                        p["maximum"]     = 5000;
                        p["description"] = QStringLiteral(
                            "Cap on cells[] (default 500), applied after the sort. "
                            "files_count stays uncapped; truncated says so.");
                        props["max_cells"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["default"]     = 5;
                        p["minimum"]     = 1;
                        p["maximum"]     = 30;
                        p["description"] = QStringLiteral(
                            "Wall-clock budget in seconds for the WHOLE anchor "
                            "set, not per run (default 5).");
                        props["timeout_sec"] = p;
                    }
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    props["etag_match"]  = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("anchors");
                    required.append("caller_cwd");
                    schema["required"]             = required;
                    schema["additionalProperties"] = false;
                    cbTool["inputSchema"] = schema;
                }
                tools.append(cbTool);

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
                    "Languages (auto-picked by extension): cpp/c (+ glsl), "
                    "py, md, json, and the brace family — rust, go, "
                    "javascript, typescript, java, csharp, kotlin, swift, "
                    "scala, php, ruby, and html. ANTS-4361: a single "
                    "self-contained HTML page — the normal shape for a small "
                    "local tool — outlines to structural LANDMARKS rather "
                    "than a DOM parse: each <style>/<script> as a region with "
                    "its start line, every element carrying an id= as an "
                    "anchor, and the top-level declarations inside each "
                    "JavaScript <script> via the brace-family parser, "
                    "reporting their real line in the FILE. A <script> whose "
                    "type is not JavaScript (application/json, text/template) "
                    "is data and is not parsed. ANTS-4090: in the brace family a "
                    "top-level `const`/`let`/`var NAME =` is emitted with "
                    "kind \"const\", so a file storing payloads in template "
                    "literals shows those regions instead of a gap. Anything "
                    "else still reports total_lines/total_bytes for "
                    "orientation. "
                    "Typically 13-39× smaller than a full Read. "
                    "ANTS-1390: pass `caller_cwd: \"~global\"` "
                    "(alias `\"~claude-config\"`) to outline files "
                    "under ~/.claude/ — for sessions editing global "
                    "Claude config (skills, agents, the global "
                    "CLAUDE.md). "
                    "ANTS-2223: pass `paths:[...]` instead of `path` to "
                    "outline several related files (a header + its impl + a "
                    "consumer) in ONE call — returns a `files:[{path, "
                    "symbols, etag, …}]` array, each entry 304ing "
                    "independently via the optional `etags` map "
                    "({relPath: priorEtag}). "
                    "NOTE (ANTS-3383): outlining a file via this verb does "
                    "NOT satisfy the native Edit tool's read-precondition — "
                    "do a native Read before editing a file you intend to "
                    "modify. "
                    "ANTS-4349: in the `paths` form, top-level `ok` means the "
                    "CALL was well-formed, NOT that any path resolved — a "
                    "partial hit is a success. Branch on `files_found` / "
                    "`files_missing` (or each entry's own `ok`), because "
                    "ok:true with files_found:0 is a total miss. "
                    "ANTS-4384: pass `sizes:true` for per-symbol `bytes` + "
                    "`lines` — which section carries the weight, and where "
                    "the seam is. "
                    "ANTS-4365: pass `raw:true` for verbatim `header_doc` / "
                    "`signature` bytes; the default framing rewrites literal "
                    "`<!--`/`-->`, so a Markdown file whose header IS an HTML "
                    "comment cannot report its own first line truthfully "
                    "without it — and an Edit built from the mangled spelling "
                    "writes it back.");
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
                    // ANTS-3800 — `generic` has been accepted by parseMode()
                    // since ANTS-2150 and was never advertised, and `glsl` is
                    // new here. Both are listed because the defect this fixes
                    // IS the schema disagreeing with what the verb accepts.
                    modeEnum.append("generic");
                    modeEnum.append("glsl");
                    modeEnum.append("html");     // ANTS-4361
                    modeProp["enum"]    = modeEnum;
                    modeProp["default"] = "auto";
                    QJsonObject hdrProp;      hdrProp["type"]      = "boolean";
                                              hdrProp["default"]   = true;
                    QJsonObject maxSymProp;   maxSymProp["type"]   = "integer";
                                              maxSymProp["default"] = 200;
                                              maxSymProp["maximum"] = 1000;
                    // ANTS-1293 — response byte cap. max_symbols bounds the
                    // count; this bounds total size. Trims symbols[] from
                    // the tail and sets truncated + symbols_dropped.
                    QJsonObject maxBytesProp; maxBytesProp["type"] = "integer";
                                              maxBytesProp["minimum"] = 1;
                                              maxBytesProp["description"] =
                        QStringLiteral("Cap on the serialized response in "
                            "bytes (default 512 KiB, server-clamped to "
                            "4 MiB). When exceeded, trailing symbols are "
                            "dropped and the envelope carries "
                            "truncated:true + symbols_dropped:<n> "
                            "(+ bytes_cap_clamped:true if the requested cap "
                            "exceeded the ceiling).");
                    // ANTS-2223 — multi-path batch. `paths` is the
                    // alternative to `path`; `etags` 304s unchanged entries.
                    QJsonObject pathsProp;    pathsProp["type"]    = "array";
                    {
                        QJsonObject items; items["type"] = "string";
                        pathsProp["items"] = items;
                    }
                    pathsProp["description"] = QStringLiteral(
                        "Repo-relative or absolute paths to outline in one "
                        "call (alternative to `path`; wins when both sent). "
                        "Returns files:[{path, symbols, etag, …}] — one entry "
                        "per path, each resolved + capped independently.");
                    QJsonObject etagsProp;    etagsProp["type"]    = "object";
                    etagsProp["description"] = QStringLiteral(
                        "Optional {project-relative-path: prior etag} map for "
                        "the `paths` form. Any file whose current etag matches "
                        "is returned as a compact {path, unchanged:true, etag} "
                        "stub instead of its full symbols — so a re-outline "
                        "after editing one file in the set re-sends only the "
                        "changed bodies.");
                    // ANTS-4384 — opt-in per-symbol extents.
                    QJsonObject sizesProp; sizesProp["type"] = "boolean";
                                           sizesProp["default"] = false;
                                           sizesProp["description"] =
                        QStringLiteral("Optional (ANTS-4384). When true, each "
                            "symbol also carries `bytes` and `lines` — its "
                            "extent from its start line to the line before the "
                            "next symbol at the SAME OR HIGHER level (EOF for "
                            "the last). So in md mode a `##` section's size "
                            "INCLUDES its `###` children, which is what makes "
                            "the answer \"where do I split this file\" rather "
                            "than \"how long is this paragraph\"; the flat "
                            "modes are sibling-scoped. Answers which section "
                            "carries the weight and where the natural seam is "
                            "without falling back to awk. Opt-in, so the "
                            "default envelope is byte-identical; composes with "
                            "compact / fields / etag_match and with the "
                            "`paths` form (uniform across the batch). A "
                            "truncated outline omits the LAST symbol's size "
                            "rather than reporting an extent that silently "
                            "absorbs everything the cap dropped.");
                    props["path"]                 = pathProp;
                    props["paths"]                = pathsProp;
                    props["etags"]                = etagsProp;
                    props["sizes"]                = sizesProp;
                    {   // ANTS-4396 — md heading-depth filter.
                        QJsonObject mh; mh["type"] = "integer";
                                        mh["minimum"] = 1;
                                        mh["maximum"] = 6;
                                        mh["description"] = QStringLiteral(
                            "Optional (ANTS-4396), md mode. Drop headings "
                            "deeper than this level — 2 keeps only `#`/`##`. "
                            "For a long append-only log (a feedback file, a "
                            "ROADMAP) the `###` entries are full sentences and "
                            "swamp the day headings that answer \"what is "
                            "here?\". NOT the same as `max_symbols`, which "
                            "truncates from the TOP and therefore keeps the "
                            "OLDEST entries — the opposite of what such a file "
                            "wants. The filter applies before the symbol "
                            "budget, so dropping deep headings frees room for "
                            "shallow ones further down. Out of range or "
                            "omitted = no filter.");
                        props["max_heading_level"] = mh;
                    }
                    {   // ANTS-3839 — symbol-name substring filter.
                        QJsonObject ft; ft["type"] = "string";
                                        ft["description"] = QStringLiteral(
                            "Optional (ANTS-3839). Keep only symbols whose "
                            "NAME contains this substring — the answer to "
                            "\"where is the roadmap stuff in this 6k-line "
                            "file?\" without paying for the whole outline. "
                            "Case-INSENSITIVE, so `outline` finds "
                            "`FileOutline`: a case-sensitive symbol filter "
                            "returns an empty list for a spelling difference, "
                            "and an unexplained empty list is what ANTS-4374 "
                            "forbids. Applied BEFORE `max_bytes` and "
                            "`max_symbols`, so it makes room rather than "
                            "competing with them. The response echoes "
                            "`filter` plus `symbols_considered` and "
                            "`symbols_filtered_out`, so zero matches is "
                            "distinguishable from a file with no symbols. "
                            "This argument was advertised by the verb's own "
                            "`leaner_call_hint` for a long time before it "
                            "existed; the hint is now true.");
                        props["filter"] = ft;
                    }
                    props["raw"]                  = makeRawProp();        // ANTS-4365
                    props["mode"]                 = modeProp;
                    props["include_doc_comment"]  = hdrProp;
                    props["max_symbols"]          = maxSymProp;
                    props["max_bytes"]            = maxBytesProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]           = makeCallerCwdReadProp();
                    props["etag_match"]           = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]               = makeFieldsProp();      // ANTS-1720
                    props["compact"]              = makeCompactProp();     // ANTS-2091
                    props["encoding"]             = makeEncodingProp();    // ANTS-2090
                    schema["properties"] = props;
                    // `path` is no longer strictly required: the `paths` form
                    // (ANTS-2223) satisfies the verb without it. The handler
                    // returns bad_path when neither is usable.
                    foTool["inputSchema"] = schema;
                }
                tools.append(foTool);

                // ANTS-4398 — mutation_probe.
                {
                    QJsonObject mp;
                    mp["name"] = "mutation_probe";
                    mp["description"] = QStringLiteral(
                        "Apply a textual mutation to a source file, run a test "
                        "command, restore the file — and REFUSE a mutation that "
                        "did not change anything. The mutate-and-watch-it-go-red "
                        "loop several projects' CLAUDE.md files mandate before "
                        "believing an invariant is held, which had no verb: "
                        "focused_test is ctest-only and does not mutate, "
                        "invariant_check reads specs and runs nothing. "
                        "`inert` is the field that matters: a mutation whose "
                        "`old` is absent, or whose replacement leaves the bytes "
                        "identical, is reported INERT and NO test is run — "
                        "running one would pass against unmutated code and read "
                        "as a weak test, which is the false conclusion this verb "
                        "exists to prevent. Outcomes: killed (the suite noticed) "
                        "| survived (it did not) | inert | timed_out | "
                        "command_not_found | write_failed. "
                        "Each mutation is applied to a CLEAN baseline so two "
                        "cannot compound, and the restore is guaranteed even on "
                        "a failed or timed-out run — `restored_clean` is "
                        "VERIFIED against the baseline bytes, because a leaked "
                        "mutated file in a repo the session then commits is the "
                        "dangerous failure. Pass require_green_baseline:true to "
                        "refuse the batch when the suite is already red, since a "
                        "mutant \"dying\" proves nothing then. "
                        "`test_command` is an ARGV ARRAY, never a shell string — "
                        "a deliberate narrowing, because this verb writes to a "
                        "source file and spawns a process. caller_cwd required.");
                    mp["selection_hint"] = QStringLiteral(
                        "Use to prove a test would actually catch a defect "
                        "before trusting it — and to find green tests that "
                        "measure nothing.");
                    {
                        QJsonObject schema;
                        schema["type"] = "object";
                        schema["required"] = QJsonArray{
                            QStringLiteral("caller_cwd"), QStringLiteral("path"),
                            QStringLiteral("mutations"),
                            QStringLiteral("test_command")};
                        QJsonObject props;
                        QJsonObject pathP; pathP["type"] = "string";
                            pathP["description"] = QStringLiteral(
                                "Project-relative source file to mutate.");
                        QJsonObject cmdP; cmdP["type"] = "array";
                            { QJsonObject it; it["type"] = "string";
                              cmdP["items"] = it; }
                            cmdP["description"] = QStringLiteral(
                                "ARGV array, e.g. [\"pytest\", \"-k\", "
                                "\"test_supervisor\"]. NOT a shell string.");
                        QJsonObject mutsP; mutsP["type"] = "array";
                            {
                                QJsonObject item; item["type"] = "object";
                                QJsonObject ip;
                                QJsonObject l; l["type"] = "string";
                                    l["description"] = QStringLiteral(
                                        "Your name for this mutant, echoed back.");
                                QJsonObject ov; ov["type"] = "string";
                                    ov["description"] = QStringLiteral(
                                        "Exact substring to replace. Absent from "
                                        "the file ⟹ inert.");
                                QJsonObject nv; nv["type"] = "string";
                                    nv["description"] = QStringLiteral(
                                        "Replacement; may be empty (a deletion "
                                        "is a mutation). Equal to `old` ⟹ inert.");
                                ip["label"] = l; ip["old"] = ov; ip["new"] = nv;
                                item["properties"] = ip;
                                mutsP["items"] = item;
                            }
                            mutsP["description"] = QStringLiteral(
                                "1-50 mutations, each run separately against a "
                                "clean baseline. Capped because each is a full "
                                "test run.");
                        QJsonObject grP; grP["type"] = "boolean";
                            grP["default"] = false;
                            grP["description"] = QStringLiteral(
                                "Run the suite unmutated first and refuse the "
                                "batch (baseline_not_green) if it is red.");
                        QJsonObject toP; toP["type"] = "integer";
                            toP["default"] = 300;
                            toP["description"] = QStringLiteral(
                                "Per-run wall-clock budget in seconds, clamped "
                                "to [5, 1800].");
                        props["path"]                   = pathP;
                        props["test_command"]           = cmdP;
                        props["mutations"]              = mutsP;
                        props["require_green_baseline"] = grP;
                        props["timeout_sec"]            = toP;
                        props["caller_cwd"] = makeCallerCwdReadProp();
                        schema["properties"] = props;
                        mp["inputSchema"] = schema;
                    }
                    tools.append(mp);
                }

                // ANTS-1855 — read_log: filter a log file, return only
                // matching lines + counts instead of Read-ing a whole
                // multi-MB log into context.
                QJsonObject rlTool;
                rlTool["name"] = "read_log";
                rlTool["description"] = QStringLiteral(
                    "Filter a log file and return only matching lines + "
                    "counts — instead of Read-ing a whole multi-MB log "
                    "into context. Default target (no `path`) is the Ants "
                    "debug log; a `path` is resolved under caller_cwd. "
                    "Filters: include/exclude (regex), contains "
                    "(substring), since (keep lines whose leading "
                    "[yyyy-MM-ddTHH:mm:ss.zzz] prefix >= this), tail "
                    "(last N). Byte-capped (max_bytes, default 512 KiB, "
                    "4 MiB ceiling) keeping the NEWEST lines. Pass "
                    "since_cursor (a prior response's `cursor`) to read "
                    "only lines appended since — token-frugal polling; a "
                    "stale/rotated cursor soft-falls-back to a full "
                    "re-read (cursor_stale:true). caller_cwd required.");
                rlTool["selection_hint"] = QStringLiteral(
                    "Use to grep a log file (esp. the Ants debug log) for "
                    "relevant lines instead of Bash grep/tail or a full "
                    "Read.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional. Log file path resolved under "
                            "caller_cwd. Omit for the Ants debug log.");
                    QJsonObject incProp; incProp["type"] = "string";
                        incProp["description"] = QStringLiteral(
                            "Optional regex; keep only matching lines.");
                    QJsonObject excProp; excProp["type"] = "string";
                        excProp["description"] = QStringLiteral(
                            "Optional regex; drop matching lines.");
                    QJsonObject cntProp; cntProp["type"] = "string";
                        cntProp["description"] = QStringLiteral(
                            "Optional literal substring a line must "
                            "contain.");
                    QJsonObject sinceProp; sinceProp["type"] = "string";
                        sinceProp["description"] = QStringLiteral(
                            "Optional. Keep lines whose leading "
                            "[timestamp] prefix is lexically >= this "
                            "(local-time yyyy-MM-ddTHH:mm:ss.zzz).");
                    QJsonObject tailProp; tailProp["type"] = "integer";
                        tailProp["minimum"] = 1; tailProp["maximum"] = 10000;
                        tailProp["description"] = QStringLiteral(
                            "Optional. Return only the last N matching "
                            "lines (clamped to 10000).");
                    QJsonObject mbProp; mbProp["type"] = "integer";
                        mbProp["minimum"] = 1;
                        mbProp["description"] = QStringLiteral(
                            "Cap on the lines[] bytes (default 512 KiB, "
                            "server-clamped to 4 MiB). Oldest lines "
                            "dropped first; sets truncated + lines_dropped "
                            "(+ bytes_cap_clamped over ceiling).");
                    QJsonObject curProp; curProp["type"] = "string";
                        curProp["description"] = QStringLiteral(
                            "Optional. Byte-offset token from a prior "
                            "response's `cursor`; reads only lines "
                            "appended since. A stale/rotated cursor "
                            "soft-falls-back (cursor_stale:true).");
                    props["path"]         = pathProp;
                    props["include"]      = incProp;
                    props["exclude"]      = excProp;
                    props["contains"]     = cntProp;
                    props["since"]        = sinceProp;
                    props["tail"]         = tailProp;
                    props["max_bytes"]    = mbProp;
                    props["since_cursor"] = curProp;
                    props["caller_cwd"]   = makeCallerCwdReadProp();
                    props["fields"]       = makeFieldsProp();   // ANTS-1720
                    props["compact"]      = makeCompactProp();   // ANTS-2091
                    schema["properties"] = props;
                    rlTool["inputSchema"] = schema;
                }
                tools.append(rlTool);

                // ANTS-2021 — read_region: return a line range or a named
                // symbol's body from a project file (ETag-304 free re-read,
                // symbol-scoped under-read) instead of a full native Read.
                QJsonObject rrTool;
                rrTool["name"] = "read_region";
                rrTool["description"] = QStringLiteral(
                    "Return an exact slice of a project file — a line range "
                    "(start_line/end_line, 1-based inclusive), a named "
                    "symbol's body (symbol), OR a markdown heading's body "
                    "(section) — instead of Read-ing the whole file. Exactly "
                    "one selector. "
                    "ANTS-4394: pass `caller_cwd: \"~global\"` (alias "
                    "`\"~claude-config\"`) to read files under ~/.claude/ — the "
                    "same sentinel file_outline (ANTS-1390) and doc_integrity "
                    "(ANTS-3719) accept. It has always worked here and was "
                    "simply undocumented, so a session reading a global "
                    "standard passed an absolute path with its project "
                    "caller_cwd, got the correct `bad_path` refusal, and fell "
                    "back to Bash sed. With the sentinel, `section=` answers "
                    "\"read § 5.4 of spec-format.md\" in one call with no line "
                    "arithmetic against a file that shifts as it is edited. Symbol mode resolves via the "
                    "flat file_outline (function/method, or a struct/class/"
                    "union aggregate whose FULL brace-matched body is returned "
                    "— ANTS-2222; a namespace still resolves to its declaration "
                    "only, use line mode for one) and can see only the first "
                    "1000 outline symbols. Section mode (ANTS-2221, .md files) "
                    "returns one heading's body up to the next same-or-higher-"
                    "level heading; pass either the heading text or its slug "
                    "(\"4.2 Emission model\" and \"4-2-emission-model\" both "
                    "resolve) — the markdown analogue of symbol mode, so you "
                    "skip the file_outline→line-arithmetic dance. "
                    "Byte-capped (max_bytes, default 512 KiB, 4 MiB ceiling), "
                    "keeping the head. ETag-304: a matching etag_match "
                    "re-read is free. caller_cwd required. NOTE (ANTS-3383): "
                    "reading via this verb does NOT satisfy the native Edit "
                    "tool's read-precondition — do a native Read before "
                    "editing a file you intend to modify.");
                rrTool["selection_hint"] = QStringLiteral(
                    "Use after find_definition/file_outline to read one "
                    "function's body or any line range instead of a full Read — "
                    "and re-read it free when unchanged. Reading several slices? "
                    "Batch them in one `read_regions` call (worth it on first "
                    "reads too).");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Project file path resolved under caller_cwd.");
                    QJsonObject startProp; startProp["type"] = "integer";
                        startProp["minimum"] = 1;
                        startProp["description"] = QStringLiteral(
                            "Line-range mode: 1-based first line. Mutually "
                            "exclusive with `symbol`.");
                    QJsonObject endProp; endProp["type"] = "integer";
                        endProp["minimum"] = 1;
                        endProp["description"] = QStringLiteral(
                            "Line-range mode: 1-based last line (inclusive); "
                            "defaults to start_line. Clamps to EOF.");
                    QJsonObject symProp; symProp["type"] = "string";
                        symProp["description"] = QStringLiteral(
                            "Symbol-body mode: return this symbol's body. "
                            "Mutually exclusive with start_line/end_line and "
                            "section.");
                    QJsonObject sectionProp; sectionProp["type"] = "string";
                        sectionProp["description"] = QStringLiteral(
                            "Section-body mode (.md, ANTS-2221): return one "
                            "markdown heading's body — the heading line through "
                            "the line before the next same-or-higher-level "
                            "heading. Accepts the heading text or its slug "
                            "(both \"4.2 Emission model\" and "
                            "\"4-2-emission-model\" resolve). A short title "
                            "also resolves a heading with a trailing "
                            "parenthetical (\"7. Build order\" matches "
                            "\"7. Build order (cheapest-first)\") when it "
                            "uniquely prefixes one heading; ambiguous prefixes "
                            "refuse with section_ambiguous + candidates "
                            "(ANTS-2234). A section_not_found refusal ALSO "
                            "carries candidates (ANTS-4350) — the same field "
                            "and shape, so one code path handles both — "
                            "ranked by word overlap first and a shared "
                            "leading section number second, capped at 10. "
                            "Mutually exclusive with the other "
                            "selectors. Echoes section + the resolved "
                            "section_slug.");
                    QJsonObject mbProp; mbProp["type"] = "integer";
                        mbProp["minimum"] = 1;
                        mbProp["description"] = QStringLiteral(
                            "Cap on the lines[] bytes (default 512 KiB, "
                            "server-clamped to 4 MiB). Keeps the head; sets "
                            "truncated (+ bytes_cap_clamped over ceiling).");
                    props["path"]       = pathProp;
                    props["start_line"] = startProp;
                    props["end_line"]   = endProp;
                    props["symbol"]     = symProp;
                    props["section"]    = sectionProp;   // ANTS-2221
                    props["max_bytes"]  = mbProp;
                    {
                        // ANTS-2157 — integration brief.
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, also return `call_sequence` (the "
                            "ordered call-expressions inside the region — a "
                            "pipeline's STAGES, each {line, callee}; line = "
                            "the insertion point) and `accessors` (the m_ "
                            "members + get/is/has getters referenced — the "
                            "helpers a new stage usually needs). Answers "
                            "\"to add a step to this pipeline, what are the "
                            "existing steps in order and what does a new one "
                            "hook into?\" in one call. Best with a `symbol` "
                            "naming the pipeline driver function.");
                        props["call_sequence"] = p;
                    }
                    props["raw"]        = makeRawProp();         // ANTS-2218
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]     = makeFieldsProp();      // ANTS-1720
                    props["compact"]    = makeCompactProp();     // ANTS-2091
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("path");
                    schema["required"] = required;
                    rrTool["inputSchema"] = schema;
                }
                tools.append(rrTool);

                // ANTS-2219 — read_regions: batched multi-selector read.
                // Fetch several slices (symbol bodies / line ranges / md
                // sections, across one or many files) in ONE call.
                QJsonObject rrsTool;
                rrsTool["name"] = "read_regions";
                rrsTool["description"] = QStringLiteral(
                    "Batched read_region: fetch several file slices in ONE "
                    "call instead of N. `items` is an array of {path, + "
                    "exactly one selector: symbol | start_line[/end_line] | "
                    "section}, each with an optional per-item etag_match. "
                    "Returns {ok, results:[…], count, truncated?} — one slice "
                    "envelope per item, in order; an item whose etag_match "
                    "matches its current slice 304s to a compact {path, "
                    "ok:true, unchanged:true, etag} stub, so a re-read after "
                    "editing one file re-sends only the changed slices. A "
                    "bad/missing item path yields a per-item {ok:false, code} "
                    "without aborting the batch. One shared max_bytes budget "
                    "(default 512 KiB, 4 MiB ceiling) is consumed in item "
                    "order. Max 64 items (too_many_items over that). "
                    "caller_cwd required. The read-side mirror of apply_edits' "
                    "batched writes — collapses \"outline → read the 6 "
                    "interesting symbols\" from 7 calls to 2. NOTE (ANTS-3383): "
                    "reading via this verb does NOT satisfy the native Edit "
                    "tool's read-precondition — do a native Read before editing "
                    "a file you intend to modify. ANTS-3500: `requests` / "
                    "`paths` / `regions` are accepted as aliases for `items`. "
                    "ANTS-3589: pass an optional top-level `path` as the "
                    "default for any item that omits its own `path` (per-item "
                    "`path` still wins) — so reading N slices of ONE file is "
                    "just {path, items:[{start_line,end_line}, …]}. "
                    "ANTS-4394: accepts `caller_cwd: \"~global\"` (alias "
                    "`\"~claude-config\"`) for files under ~/.claude/, the same "
                    "sentinel read_region and file_outline take.");
                rrsTool["selection_hint"] = QStringLiteral(
                    "Use when one file_outline/find_definition pass surfaced "
                    "several symbols/sections to read together — batch them "
                    "here instead of one read_region per slice.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject itemsProp;
                    itemsProp["type"] = "array";
                    itemsProp["description"] = QStringLiteral(
                        "Slices to fetch (1-64). Each item: a `path` plus "
                        "exactly one selector — `symbol` (a function/aggregate "
                        "body), `start_line` (+ optional `end_line`) for a "
                        "line range, or `section` (a markdown heading "
                        "slug/text). Optional per-item `etag_match` 304s an "
                        "unchanged slice.");
                    QJsonObject itemSchema;
                    itemSchema["type"] = "object";
                    itemSchema["additionalProperties"] = false;
                    QJsonObject ip;
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Project file path resolved under caller_cwd.");
                      ip["path"] = p; }
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Symbol-body selector (function/method or "
                          "struct/class/union aggregate). Mutually exclusive "
                          "with start_line/end_line/section.");
                      ip["symbol"] = p; }
                    { QJsonObject p; p["type"] = "integer"; p["minimum"] = 1;
                      p["description"] = QStringLiteral(
                          "Line-range selector: 1-based first line.");
                      ip["start_line"] = p; }
                    { QJsonObject p; p["type"] = "integer"; p["minimum"] = 1;
                      p["description"] = QStringLiteral(
                          "Line-range selector: 1-based last line "
                          "(inclusive); defaults to start_line.");
                      ip["end_line"] = p; }
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Markdown section selector (.md): a heading slug or "
                          "text (ANTS-2221).");
                      ip["section"] = p; }
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Per-item ETag: when it equals this item's current "
                          "slice etag, the item 304s to an {unchanged:true} "
                          "stub.");
                      ip["etag_match"] = p; }
                    itemSchema["properties"] = ip;
                    QJsonArray itemReq; itemReq.append("path");
                    itemSchema["required"] = itemReq;
                    itemsProp["items"] = itemSchema;
                    props["items"] = itemsProp;
                    { QJsonObject p; p["type"] = "integer"; p["minimum"] = 1;
                      p["description"] = QStringLiteral(
                          "Shared cap on total results[] bytes across the set "
                          "(default 512 KiB, 4 MiB ceiling), consumed in item "
                          "order.");
                      props["max_bytes"] = p; }
                    props["raw"]        = makeRawProp();         // ANTS-2218
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    // ANTS-3568 — declare requests/paths/regions as schema
                    // properties so additionalProperties:false does not strip
                    // these documented aliases (ANTS-3500) before they reach
                    // cmdReadRegions' fallback. Same array shape as `items`
                    // (the handler picks the first array-valued alias when
                    // `items` is absent); `items` still wins.
                    for (const char *alias : {"requests", "paths", "regions"}) {
                        QJsonObject a = itemsProp;
                        a["description"] = QStringLiteral(
                            "Alias for `items` (ANTS-3500) — same array shape. "
                            "Declared so the documented alias survives schema "
                            "validation; canonical `items` still wins.");
                        props[QLatin1String(alias)] = a;
                    }
                    // ANTS-3589 — optional top-level `path`: the per-item
                    // default for any item that omits its own `path`. Declared
                    // so additionalProperties:false keeps it (and so it is not
                    // reported in ignored_args). Per-item `path` still wins.
                    { QJsonObject p; p["type"] = "string";
                      p["description"] = QStringLiteral(
                          "Optional default path (ANTS-3589): any item that "
                          "omits its own `path` reads from this file instead, "
                          "so reading N slices of one file need not repeat the "
                          "filename. A per-item `path` still wins.");
                      props["path"] = p; }
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("items");
                    schema["required"] = required;
                    rrsTool["inputSchema"] = schema;
                }
                tools.append(rrsTool);

                // ANTS-2094 — read_spill: re-read a body the offload path
                // spilled to a content-addressed cache file, by its handle,
                // byte-paged. Pairs with the offload envelope's `hint`.
                QJsonObject rsTool;
                rsTool["name"] = "read_spill";
                rsTool["description"] = QStringLiteral(
                    "Re-read a large result that was offloaded (observation "
                    "masking) — when a read verb returned {offloaded:true, "
                    "handle, head, ...} instead of the full body, fetch the "
                    "full body here by `handle`, byte-paged. Args: handle "
                    "(required, the 64-hex sha256 from the envelope), optional "
                    "offset (0-based byte offset, default 0) and max_bytes "
                    "(default 512 KiB, 4 MiB ceiling). Page by advancing to the "
                    "RETURNED offset+bytes (a UTF-8 char-boundary cut can "
                    "shorten a slice). Returns {content, offset, bytes, "
                    "total_bytes, truncated}. ANTS-3545 — for an offloaded body "
                    "whose preview showed head_rows_key (an array of rows), "
                    "pass row_offset/row_count instead to page that array by "
                    "ROW, parsed: returns a DIFFERENT envelope {mode:\"rows\", "
                    "ANTS-4397: an offload envelope whose row BODIES do not "
                    "all fit carries `rows_preview` — one shape row per row, "
                    "{index, bytes, head} — instead of a prefix that shows one "
                    "row and says nothing about the rest. Use `bytes` to pick "
                    "what to fetch; `rows_preview_heads_omitted` means the "
                    "text samples were dropped so EVERY row could be covered. "
                    "`head_rows` is unchanged and still the right preview for "
                    "many small rows. "
                    "key, rows, row_offset, total_rows, population, truncated} (no "
                    "content); page on via row_offset + rows.size(). Refusals: "
                    "bad_args (bad handle, or negative "
                    "offset/max_bytes/row_offset/row_count), "
                    "not_found (never spilled or evicted — re-issue the "
                    "original call), too_large / not_array (row mode, on a "
                    "> 1 MiB or non-array body — byte-page it instead). The "
                    "spill store is global/content-"
                    "addressed, so caller_cwd is optional.");
                rsTool["selection_hint"] = QStringLiteral(
                    "Use only after a read verb returned an offloaded:true "
                    "envelope, to fetch the rest of the body via its handle — "
                    "byte-paged, or (if the preview showed head_rows_key) "
                    "row-paged with row_offset/row_count.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject hP; hP["type"] = "string";
                        hP["description"] = QStringLiteral(
                            "Content-addressed handle from the offload "
                            "envelope: a bare lowercase 64-char sha256.");
                    QJsonObject oP; oP["type"] = "integer";
                        oP["minimum"] = 0;
                        oP["description"] = QStringLiteral(
                            "0-based byte offset into the body (default 0). "
                            "Page by the RETURNED offset+bytes.");
                    QJsonObject mbP; mbP["type"] = "integer";
                        mbP["minimum"] = 1;
                        mbP["description"] = QStringLiteral(
                            "Cap on the returned slice in bytes (default "
                            "512 KiB, server-clamped to 4 MiB).");
                    // ANTS-3545 — row-paging args (row mode). integer,
                    // minimum:0 (matching byte-mode offset); a numeric value
                    // routes cmdReadSpill to readSpillRows.
                    QJsonObject roP; roP["type"] = "integer";
                        roP["minimum"] = 0;
                        roP["description"] = QStringLiteral(
                            "Row-paging: 0-based index of the first row of the "
                            "offloaded body's dominant array (the head_rows_key "
                            "array). Presence of row_offset or row_count "
                            "switches read_spill to row mode; page on by "
                            "row_offset + rows.size().");
                    QJsonObject rcP; rcP["type"] = "integer";
                        rcP["minimum"] = 0;
                        rcP["description"] = QStringLiteral(
                            "Row-paging: number of rows to return (default 100; "
                            "0/omitted = default). Row mode returns "
                            "ANTS-4375: `total_rows` is the rows IN the "
                            "spill file, which is what paging is over; "
                            "`population` is what the producing verb was "
                            "reporting on, and `rows_are_partial:true` fires "
                            "when the array was capped BEFORE spilling — then "
                            "paging to the end is NOT completeness, however "
                            "`truncated` reads. "
                            "{mode:\"rows\", key, rows, row_offset, total_rows, "
                            "truncated}.");
                    props["handle"]     = hP;
                    props["offset"]     = oP;
                    props["max_bytes"]  = mbP;
                    props["row_offset"] = roP;     // ANTS-3545
                    props["row_count"]  = rcP;     // ANTS-3545
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("handle");
                    schema["required"] = required;
                    rsTool["inputSchema"] = schema;
                }
                tools.append(rsTool);

                // ANTS-2022 — apply_edits: apply N {path, old, new} edits
                // across M project files in one atomic-per-file call,
                // instead of one native Edit round-trip per site.
                QJsonObject aeTool;
                aeTool["name"] = "apply_edits";
                aeTool["description"] = QStringLiteral(
                    "Apply a batch of {path, old, new} edits across one or "
                    "more project files in ONE call — instead of a native "
                    "Edit round-trip per site. Each edit replaces `old` with "
                    "`new` in `path`; without replace_all, `old` must occur "
                    "exactly once (else the edit is skipped: not_found for 0, "
                    "ambiguous for >1). Atomic per file (QSaveFile); multiple "
                    "edits to one file apply in array order. A path escaping "
                    "the project root fails the whole call (bad_path) — with "
                    "ONE deliberate exception (ANTS-3430/3616): a "
                    "`*_Ants_MCP_Feedback.md` file sitting directly in the "
                    "project root's PARENT directory is reachable, because "
                    "that shared cross-session file lives outside every "
                    "project by convention. A "
                    "missing file / absent-or-ambiguous old / >4 MiB file / "
                    "failed commit is a per-edit skip. Returns applied[] (one "
                    "per file) + skipped[] (one per edit) + counts. "
                    "caller_cwd required. "
                    "LINE RANGES (ANTS-3711): an edit may instead name an "
                    "inclusive 1-based `start_line`/`end_line` range — use it "
                    "to replace or delete a large contiguous block without "
                    "re-emitting bytes you already read (`old` and a range "
                    "together → bad_args). A range edit MUST also carry "
                    "`expect_first_line`/`expect_last_line`, the verbatim text "
                    "of those two lines: `old` guards itself by being unique, "
                    "a line number guards nothing, so without them a stale "
                    "number silently replaces the wrong lines. Mismatch → a "
                    "`range_mismatch` skip; out-of-file coordinates → "
                    "`range_out_of_bounds`. read_region hands back both the "
                    "numbers and the text. Ranges resolve against the file as "
                    "EARLIER edits in the same call left it, so a batch that "
                    "shifts line counts will trip the guard — order "
                    "line-shifting edits last, or send them bottom-up. "
                    "BATCH SIZE (ANTS-3712): the 4 MiB above is a cap on the "
                    "FILE being edited, NOT on this call's arguments — the "
                    "request itself is far smaller-bounded by the client's "
                    "tool-call transport. A batch whose combined old+new runs "
                    "to several KB can fail BEFORE this server sees it, as a "
                    "client-side `could not be parsed as JSON` whose hints "
                    "blame escaping; the byte count it echoes is the real "
                    "clue. If you hit that, split edits[] into smaller calls "
                    "rather than auditing your escaping — identical content "
                    "applies cleanly in halves. dry_run cannot diagnose it "
                    "(the call never arrives).");
                aeTool["selection_hint"] = QStringLiteral(
                    "Use for a multi-site sweep (same change across N files) "
                    "to collapse N native Edit calls into one atomic batch.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject editsProp; editsProp["type"] = "array";
                        editsProp["description"] = QStringLiteral(
                            "Non-empty list of edits. Each is {path, new} plus "
                            "EITHER {old, replace_all?} OR (ANTS-3711) "
                            "{start_line, end_line, expect_first_line, "
                            "expect_last_line} — never both. ANTS-4089: "
                            "old_string/new_string are accepted as aliases for "
                            "old/new, so the native Edit tool's spelling works "
                            "here unchanged.");
                    QJsonObject items; items["type"] = "object";
                        items["additionalProperties"] = false;
                        QJsonObject ip;
                        QJsonObject pP; pP["type"] = "string";
                            pP["description"] = QStringLiteral(
                                "File path resolved under caller_cwd.");
                        QJsonObject oP; oP["type"] = "string";
                            oP["description"] = QStringLiteral(
                                "Substring to replace (non-empty; must be "
                                "unique unless replace_all). Omit when using "
                                "a start_line/end_line range instead.");
                        QJsonObject nP; nP["type"] = "string";
                            nP["description"] = QStringLiteral(
                                "Replacement text (may be empty for a "
                                "deletion).");
                        QJsonObject rP; rP["type"] = "boolean";
                            rP["description"] = QStringLiteral(
                                "Replace every occurrence of `old` (default "
                                "false → require a unique match).");
                        // ANTS-3711 — the line-range alternative to `old`.
                        QJsonObject slP; slP["type"] = "integer";
                            slP["minimum"] = 1;
                            slP["description"] = QStringLiteral(
                                "First line of the range to replace (1-based, "
                                "inclusive). Alternative to `old`; requires "
                                "end_line + both expect_* lines.");
                        QJsonObject elP; elP["type"] = "integer";
                            elP["minimum"] = 1;
                            elP["description"] = QStringLiteral(
                                "Last line of the range to replace (1-based, "
                                "inclusive; may equal start_line).");
                        QJsonObject efP; efP["type"] = "string";
                            efP["description"] = QStringLiteral(
                                "Verbatim text of line start_line. Mandatory "
                                "on a range edit — it is the range's only "
                                "staleness guard. Mismatch → range_mismatch "
                                "skip, no write.");
                        QJsonObject elnP; elnP["type"] = "string";
                            elnP["description"] = QStringLiteral(
                                "Verbatim text of line end_line. Mandatory on "
                                "a range edit; checked together with "
                                "expect_first_line so a range that shifted at "
                                "either end is caught.");
                        // ANTS-4089 — old_string/new_string as aliases for
                        // old/new, because that is the spelling every session
                        // arrives holding from the native Edit tool. They have
                        // to be DECLARED: additionalProperties is false, so an
                        // undeclared key is refused before the handler sees it.
                        QJsonObject osP; osP["type"] = "string";
                            osP["description"] = QStringLiteral(
                                "Alias for `old` — the native Edit tool's "
                                "spelling, accepted so this verb is a drop-in "
                                "for it. `old` wins if both are sent.");
                        QJsonObject nsP; nsP["type"] = "string";
                            nsP["description"] = QStringLiteral(
                                "Alias for `new` — the native Edit tool's "
                                "spelling. `new` wins if both are sent.");
                        ip["path"] = pP; ip["old"] = oP; ip["new"] = nP;
                        ip["old_string"] = osP; ip["new_string"] = nsP;
                        ip["replace_all"] = rP;
                        ip["start_line"] = slP; ip["end_line"] = elP;
                        ip["expect_first_line"] = efP;
                        ip["expect_last_line"]  = elnP;
                        items["properties"] = ip;
                        // Neither `old` nor `new` is unconditionally required:
                        // an edit satisfies the handler with either selector
                        // and either spelling, and both rules are enforced
                        // there (ANTS-3711, ANTS-4089). `new` left in required
                        // would refuse every new_string call before it landed.
                        QJsonArray ir; ir.append("path");
                        items["required"] = ir;
                    editsProp["items"] = items;
                    props["edits"]      = editsProp;
                    props["dry_run"]    = makeDryRunProp();      // ANTS-2227
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray required;
                    required.append("edits");
                    schema["required"] = required;
                    aeTool["inputSchema"] = schema;
                }
                tools.append(aeTool);

                // ANTS-1637 — codebase_index: serve a pre-computed project
                // structural map (symbols-per-file, lane→files) so a session
                // stops re-deriving the project shape with grep/file_outline.
                QJsonObject ciTool;
                ciTool["name"] = "codebase_index";
                ciTool["description"] = QStringLiteral(
                    "Pre-computed project structural map, cached + lazily "
                    "refreshed. No selector → a summary "
                    "(file_count / lanes / languages / roles); add "
                    "lane_files:true for a compact per-lane source_files digest "
                    "(navigable lane→file map). symbol=Foo::bar "
                    "→ every {path,line,kind} defining it (pre-indexed "
                    "find_definition, no re-grep). lane=<name> → that lane's "
                    "files + their symbols. file_path=<rel> → one file's cached "
                    "outline. At most one selector (≥2 → bad_args). A miss is "
                    "ok:true,found:false (not an error). Symbol coverage = "
                    "file_outline coverage. ETag-304: an unchanged query "
                    "re-reads free. caller_cwd required.");
                ciTool["selection_hint"] = QStringLiteral(
                    "Use once at session start for the project's shape, or to "
                    "locate a symbol/lane, instead of repeated grep / "
                    "file_outline / CLAUDE.md reads.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject symProp; symProp["type"] = "string";
                        symProp["description"] = QStringLiteral(
                            "Exact, case-sensitive symbol name. Mutually "
                            "exclusive with lane / file_path.");
                    QJsonObject laneProp; laneProp["type"] = "string";
                        laneProp["description"] = QStringLiteral(
                            "Subsystem lane name. Mutually exclusive with "
                            "symbol / file_path.");
                    QJsonObject fpProp; fpProp["type"] = "string";
                        fpProp["description"] = QStringLiteral(
                            "Project file path (under caller_cwd). Mutually "
                            "exclusive with symbol / lane.");
                    QJsonObject laneFilesProp; laneFilesProp["type"] = "boolean";
                        laneFilesProp["description"] = QStringLiteral(
                            "Summary-only opt-in: also emit a compact per-lane "
                            "source_files digest (each lane's non-test paths, "
                            "sorted, globally capped; lane_digest_truncated "
                            "flags a cap hit) — a navigable lane→file map "
                            "without a per-lane call. Ignored under a selector. "
                            "(ANTS-3468) ANTS-3503: a project with no parseable "
                            "`## Module map` (empty lane digest) instead gets a "
                            "flat top-level `source_files` digest (same cap/flag) "
                            "so lane-less repos still get a first-call code map.");
                    props["symbol"]     = symProp;
                    props["lane"]       = laneProp;
                    props["file_path"]  = fpProp;
                    props["lane_files"] = laneFilesProp;   // ANTS-3468
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]     = makeFieldsProp();      // ANTS-1720
                    props["compact"]    = makeCompactProp();     // ANTS-2091
                    props["encoding"]   = makeEncodingProp();    // ANTS-2090
                    schema["properties"] = props;
                    ciTool["inputSchema"] = schema;
                }
                tools.append(ciTool);

                // ANTS-2139 — docs_index: serve a pre-computed project
                // documentation map (heading outline + title + status +
                // relative-link graph) so a session finds the right doc in
                // one call instead of grep/Read across an unfamiliar layout.
                QJsonObject diTool;
                diTool["name"] = "docs_index";
                diTool["description"] = QStringLiteral(
                    "Pre-computed project documentation map, cached + lazily "
                    "refreshed. Project-agnostic (any layout, not just Ants). "
                    "No selector → a summary (per-doc {path,id,title,status,"
                    "heading_count} + dir rollup). topic=<words> → docs scored "
                    "over title/path/headings, ranked, with evidence. "
                    "doc_path=<rel> → one doc's heading outline + outbound "
                    "links + linked_from reverse edges. id=<stem> → every doc "
                    "whose filename stem matches. At most one selector (≥2 → "
                    "bad_args). A miss is ok:true,found:false (not an error). "
                    "Indexes headings/title/path, not full body prose. "
                    "ETag-304: an unchanged query re-reads free. caller_cwd "
                    "required.");
                diTool["selection_hint"] = QStringLiteral(
                    "Use once to find which doc covers a topic, or to read a "
                    "doc's outline, instead of repeated grep / Read. The "
                    "SOURCE-map sibling is codebase_index; spec_query stays "
                    "the deep per-spec reader.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject topicProp; topicProp["type"] = "string";
                        topicProp["description"] = QStringLiteral(
                            "Whitespace-separated keywords; scored over "
                            "title/path/headings. Mutually exclusive with "
                            "doc_path / id.");
                    QJsonObject dpProp; dpProp["type"] = "string";
                        dpProp["description"] = QStringLiteral(
                            "Project doc path (under caller_cwd) for one doc's "
                            "outline. Mutually exclusive with topic / id.");
                    QJsonObject idProp; idProp["type"] = "string";
                        idProp["description"] = QStringLiteral(
                            "Exact, case-sensitive filename stem (e.g. "
                            "\"ANTS-1637\"). Mutually exclusive with topic / "
                            "doc_path.");
                    props["topic"]      = topicProp;
                    props["doc_path"]   = dpProp;
                    props["id"]         = idProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"]     = makeFieldsProp();      // ANTS-1720
                    props["encoding"]   = makeEncodingProp();    // ANTS-2090
                    schema["properties"] = props;
                    diTool["inputSchema"] = schema;
                }
                tools.append(diTool);

                // ANTS-3601 — doc_integrity: deterministic dead-anchor /
                // broken-link / TOC-coverage checks over a doc set. The
                // grep-able rot cold-eyes reviewers otherwise find by hand.
                QJsonObject docInt;
                docInt["name"] = "doc_integrity";
                docInt["description"] = QStringLiteral(
                    "Deterministic markdown doc-integrity check (no LLM). Reports "
                    "these kinds: dead_anchor (a [t](#slug) / [t](other.md#slug) "
                    "naming no real heading), broken_link (a [t](relpath) whose "
                    "target file is missing), toc_gap (a hand-maintained Table of "
                    "Contents that omits an H2 section or lists a duplicate), and "
                    "heading_sequence (ANTS-3700 — a numbered heading like "
                    "`## 5.7 Foo` that is lower than the sibling before it, skips "
                    "a number no sibling ever fills, or repeats one; siblings are "
                    "grouped by numeric parent prefix, a group's first heading is "
                    "never flagged, and unnumbered/prose headings are untouched), "
                    "and ungranted_tool (ANTS-3719 — a Claude Code skill whose "
                    "body calls `mcp__ants__<verb>` while its own allowed-tools "
                    "frontmatter never granted it, so the skill is unexecutable "
                    "as written; gated on the frontmatter carrying allowed-tools, "
                    "which scopes it to skill files, and matched on the "
                    "fully-qualified spelling only). "
                    "Fence-aware (fenced examples ignored); GitHub-compatible "
                    "heading slugs. path=<file|dir> scopes the run (a dir walks "
                    "its *.md recursively); paths=[...] scopes it to the UNION "
                    "of several files/dirs and wins over path (ANTS-4106 — a "
                    "post-fix sweep over the files a run edited, which no "
                    "single path covers); omitted → the project docs_dir (else "
                    "docs/). kinds=[...] filters findings + counts. A non-existent "
                    "in-root path is ok:true with empty checked_docs; a "
                    "root-escaping path refuses bad_path. Emits docs_digest — a "
                    "fingerprint of the checked set (ANTS-3737) — so the ETag "
                    "tracks the DOCUMENTS, not just the findings: before that, "
                    "editing docs without changing a finding left the envelope "
                    "identical and etag_match returned a false 304, skipping the "
                    "post-fix re-check. ETag-304: unchanged docs re-read free. "
                    "caller_cwd required — or the `~global` / `~claude-config` "
                    "sentinel (ANTS-3719) to check the global Claude config tree "
                    "at ~/.claude, which has no docs/ dir, so pass an explicit "
                    "path there (e.g. path=\"skills\").");
                docInt["selection_hint"] = QStringLiteral(
                    "Use before a cold-eyes doc review, or after editing a long "
                    "contract doc, to catch dead anchors / broken links / TOC "
                    "drift mechanically. The literal-drift sibling is the "
                    "contract_doc_drift audit lane.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Project-relative file or directory to check "
                            "(default: the docs_dir override, else docs/). A "
                            "directory walks its *.md recursively.");
                    QJsonObject kindsProp; kindsProp["type"] = "array";
                        {
                            QJsonObject items; items["type"] = "string";
                            items["enum"] = QJsonArray{
                                QStringLiteral("dead_anchor"),
                                QStringLiteral("broken_link"),
                                QStringLiteral("toc_gap"),
                                QStringLiteral("heading_sequence"),
                                QStringLiteral("ungranted_tool")};
                            kindsProp["items"] = items;
                        }
                        kindsProp["description"] = QStringLiteral(
                            "Optional filter; narrows findings AND counts to "
                            "these kinds. Omitted → every kind.");
                    // ANTS-4106 — the multi-file form, matching file_outline's
                    // `paths` and read_regions' slice list.
                    QJsonObject pathsProp; pathsProp["type"] = "array";
                        {
                            QJsonObject items; items["type"] = "string";
                            pathsProp["items"] = items;
                        }
                        pathsProp["description"] = QStringLiteral(
                            "Project-relative files and/or directories to check "
                            "in ONE run, findings reported over their union "
                            "(ANTS-4106). Wins over `path` when both are sent. "
                            "Use it to scope a post-fix sweep to the files a "
                            "run actually edited — a real pass edits a handful "
                            "spread across the tree, which no single `path` "
                            "covers, so the whole-tree walk was what happened "
                            "and a pre-existing finding in an untouched file "
                            "got attributed to the pass. docs_digest and the "
                            "ETag key off the union, as they key off the walked "
                            "set today. A root-escaping entry refuses "
                            "bad_path.");
                    props["path"]       = pathProp;
                    props["paths"]      = pathsProp;   // ANTS-4106
                    props["kinds"]      = kindsProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"] = props;
                    docInt["inputSchema"] = schema;
                }
                tools.append(docInt);

                // ANTS-3661 — doc_symbols: resolve the identifiers a doc
                // asserts something about. Reports; never judges.
                QJsonObject docSym;
                docSym["name"] = "doc_symbols";
                docSym["description"] = QStringLiteral(
                    "Resolve the identifiers a doc claims exist. Harvests inline code spans "
                    "that look like `Foo::bar()` and looks each up with find_definition's "
                    "resolver, reporting resolved | unresolved | not_checked per occurrence. "
                    "Fenced code, paths, language keywords, MCP verb/argument names and "
                    "doc-examples regions are excluded. A bare lowercase word (no ::, no (), "
                    "no case boundary) is reported ONLY if it resolves — unresolved it is "
                    "indistinguishable from a JSON key or a config key, so it is dropped "
                    "rather than guessed at. REPORT-ONLY: an unresolved name may be rot or a "
                    "forward reference to something the doc is about to create, and deciding "
                    "which is yours — the verb emits no severity and nothing auto-fixable. "
                    "not_checked means a needle the run never looked up (cap or deadline), "
                    "never 'does not exist'; truncated:true accompanies it. Emits "
                    "docs_digest, a fingerprint of the checked set, so the ETag tracks the "
                    "documents and not just the findings (ANTS-3737). "
                    "ANTS-4359: the unresolved bucket is classified by the SHAPE the "
                    "document wrote — each finding carries `shape` (call | qualified | "
                    "bare) and `counts` carries `unresolved_by_shape`. Read the "
                    "call-shaped ones FIRST: a span with `()` is a claim about a "
                    "FUNCTION, where \"no such symbol\" is usually a real defect, while "
                    "bare identifiers are as often enum values, macros, CMake variables "
                    "or JSON keys. A classification, never a verdict — nothing is "
                    "filtered and no bucket is dropped. Read-only. "
                    "caller_cwd required.");
                docSym["selection_hint"] = QStringLiteral(
                    "Use when reviewing a spec or design doc to get the short list of names "
                    "it mentions that resolve nowhere — the expensive half of a cold read.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    QJsonObject props;
                    QJsonObject dsPath; dsPath["type"] = "string";
                        dsPath["description"] = QStringLiteral(
                            "Project-relative file or directory to scan (a directory walks "
                            "*.md recursively). Default: the project's docs dir, else docs/.");
                    QJsonObject dsCwd; dsCwd["type"] = "string";
                        dsCwd["description"] = QStringLiteral(
                            "Your $PWD. Required — anchors the doc walk and the symbol "
                            "resolution to your project.");
                    QJsonObject dsEtag; dsEtag["type"] = "string";
                        dsEtag["description"] = QStringLiteral(
                            "Server-issued etag from a prior call; an unchanged corpus "
                            "short-circuits to {ok:true, unchanged:true}.");
                    props["path"] = dsPath;
                    props["caller_cwd"] = dsCwd;
                    props["etag_match"] = dsEtag;
                    schema["properties"] = props;
                    docSym["inputSchema"] = schema;
                }
                tools.append(docSym);

                // ANTS-3662 — spec_lint: the greppable half of the spec-format
                // contract, which /cold-eyes § 1e hand-rolls every review pass.
                QJsonObject specLint;
                specLint["name"] = "spec_lint";
                specLint["description"] = QStringLiteral(
                    "Check specs for the structural defects /cold-eyes § 1e greps for by "
                    "hand: an INV-N with no test-surface clause (invariant_no_test), a gap "
                    "in the doc's own id sequence (invariant_id_gap), a cold-eyes loop-log "
                    "row with no outcome (loop_row_no_outcome), and a test clause that is a "
                    "command stating nothing it should return (command_test_no_expectation, "
                    "a CANDIDATE — never auto-fixable, and no subprocess is ever run). "
                    "Tombstoned invariants (*moved to X*, *withdrawn — …*) are exempt from "
                    "both invariant checks, ANTS-4351: however the body is WRAPPED (the "
                    "body is joined before matching, as a reader does — a hard-wrapped "
                    "tombstone used to come back as invariant_no_test, byte-identical to a "
                    "genuinely untested invariant). missing_section runs ONLY when the "
                    "project's format standard carries a <!-- required-sections --> block; "
                    "without one it is skipped and sections_checked comes back false, which "
                    "is the shipping default — a check against a guessed list would fire on "
                    "every conforming spec. ANTS-4390: the standard is looked for at "
                    "docs/standards/{spec-format,specs}.md and then standards/{...}.md, so a "
                    "repository that IS the standards set (no docs/ prefix) can check its "
                    "own specs. ANTS-4080: those same four are then tried under ~/.claude/, "
                    "where the spec-format standard has been authoritative since 2026-08-08 "
                    "— so a project carrying only a delta is no longer linted against "
                    "nothing, and sections_source prefixes the global hit with `~global/`. "
                    "ANTS-3784: a document may declare a DELIBERATE id floor with an "
                    "<!-- invariant-id-base: N --> line outside fenced code; missing numbers "
                    "below N are counted as suppressed rather than reported, which is the "
                    "only route for a spec that continues a sibling's numbering on a corpus "
                    "that otherwise numbers per-document. ANTS-4373: when the check is skipped the envelope says so "
                    "in fields a caller reads rather than in a boolean it does not — "
                    "skipped[] naming EVERY gated check that did not run plus a "
                        "skipped_hint naming the paths "
                    "consulted; sections_source names the standard that DID resolve (null "
                    "when none did). Treat a skipped run as SILENT about section structure, "
                    "never as a clean structural result. Size is reported in line_count, never as a finding. "
                    "Emits docs_digest, a fingerprint of the checked set, so the ETag "
                    "tracks the documents and not just the findings (ANTS-3737). "
                    "Read-only. caller_cwd required.");
                specLint["selection_hint"] = QStringLiteral(
                    "Use before a spec review to clear the mechanical findings, so reviewer "
                    "attention goes to judgement rather than to grep-able trivia.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    QJsonObject props;
                    QJsonObject slPath; slPath["type"] = "string";
                        slPath["description"] = QStringLiteral(
                            "Project-relative spec file or directory (a directory walks *.md "
                            "recursively). Default: the project's specs dir, else docs/specs/.");
                    QJsonObject slCwd; slCwd["type"] = "string";
                        slCwd["description"] = QStringLiteral(
                            "Your $PWD. Required — anchors the spec walk and the format-"
                            "standard lookup to your project.");
                    QJsonObject slMax; slMax["type"] = "integer";
                        slMax["description"] = QStringLiteral(
                            "Run-wide cap on findings[] (default 500, clamped to [1,5000]). "
                            "truncated:true accompanies a capped run.");
                    QJsonObject slEtag; slEtag["type"] = "string";
                        slEtag["description"] = QStringLiteral(
                            "Server-issued etag from a prior call; an unchanged corpus "
                            "short-circuits to {ok:true, unchanged:true}.");
                    props["path"] = slPath;
                    props["caller_cwd"] = slCwd;
                    props["max_findings"] = slMax;
                    props["etag_match"] = slEtag;
                    schema["properties"] = props;
                    specLint["inputSchema"] = schema;
                }
                tools.append(specLint);

                // ANTS-4108 — spec_conformance: the EXECUTABLE half of spec
                // review. spec_lint greps a spec's structure; this RUNS the
                // patterns it prescribes against the examples beside them.
                QJsonObject specConf;
                specConf["name"] = "spec_conformance";
                specConf["description"] = QStringLiteral(
                    "Run a spec's own regex patterns against the `| input | expected |` "
                    "table beside them, instead of reading them. Three buckets: a row "
                    "whose actual result differs from `expected` is a FINDING; a "
                    "```regex fence with no table beside it is a CANDIDATE; a per-case "
                    "timing is an OBSERVATION (which is what catches a fixture that "
                    "returns before the pattern under test ever runs). Executes "
                    "PATTERNS ONLY — never fenced code, so nothing it runs can reach "
                    "the filesystem or the network. Engine tag `pcre2` only; anything "
                    "else refuses `unsupported_engine` per fence rather than "
                    "substituting an engine. Refusals: bad_args (absent `path`, "
                    "`max_cases` outside [1,1000] — refused, never clamped), bad_path, "
                    "not_found. "
                    "ANTS-4370: a fence this verb cannot execute is reported "
                    "rather than ignored — `skipped_fences[]` names each one "
                    "with its tag and line, and `executable_fences` is the "
                    "denominator beside `cases_run`. A spec whose patterns sit "
                    "in ```python fences used to return EVERY bucket empty, "
                    "which reads as a pass; `executable_fences:0` with a "
                    "non-empty `skipped_fences[]` means this run checked "
                    "NOTHING. Those fences are still not candidates — that is "
                    "§ 2.6 on purpose, since every code block in every spec "
                    "would otherwise be a finding. "
                    "Read-only. caller_cwd required; `path` required.");
                specConf["selection_hint"] = QStringLiteral(
                    "Use on a spec that prescribes regexes, before implementing it: a "
                    "cold read passes a wrong pattern, running it does not.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd"),
                                                    QStringLiteral("path")};
                    QJsonObject props;
                    QJsonObject scPath; scPath["type"] = "string";
                        scPath["description"] = QStringLiteral(
                            "Project-relative path to ONE markdown document. Required — "
                            "this verb executes a document, so there is no tree walk to "
                            "default to (unlike spec_lint).");
                    QJsonObject scCwd; scCwd["type"] = "string";
                        scCwd["description"] = QStringLiteral(
                            "Your $PWD. Required — anchors the path under your project.");
                    QJsonObject scMax; scMax["type"] = "integer";
                        scMax["description"] = QStringLiteral(
                            "Cap on executed cases (default 200, range [1,1000]). Out of "
                            "range REFUSES bad_args rather than clamping; a capped run "
                            "returns truncated:true so a partial run never reads as a "
                            "complete one.");
                    QJsonObject scEtag; scEtag["type"] = "string";
                        scEtag["description"] = QStringLiteral(
                            "Server-issued etag from a prior call; an unchanged document "
                            "short-circuits to {ok:true, unchanged:true}. The etag covers "
                            "the envelope minus observations[] (per-run timings), so it "
                            "tracks the answer and not the clock.");
                    props["path"] = scPath;
                    props["caller_cwd"] = scCwd;
                    props["max_cases"] = scMax;
                    props["etag_match"] = scEtag;
                    schema["properties"] = props;
                    specConf["inputSchema"] = schema;
                }
                tools.append(specConf);

                // ANTS-3660 — doc_dedup: the same passage written twice, which
                // /cold-eyes § 1e has no mechanical check for at all.
                QJsonObject docDedup;
                docDedup["name"] = "doc_dedup";
                docDedup["description"] = QStringLiteral(
                    "Find near-duplicate PASSAGES across a doc set — the same fact written "
                    "twice, which /cold-eyes Phase 4 names as the usual cause of a review "
                    "that will not converge. Segments each markdown file into paragraphs "
                    "(a list item is one passage, marker line included), shingles them into "
                    "word 3-grams and reports every pair at or above min_similarity as "
                    "Jaccard. Fence-aware: two identical code samples are not a duplicated "
                    "fact. pairs[] carries both ends ({a,b,similarity}); clusters[] groups "
                    "them into connected components, which is the readable view — N docs "
                    "sharing one stanza are N(N-1)/2 pairs but ONE thing to fix. "
                    "REPORT-ONLY and never auto-fixable: which copy is canonical is a "
                    "question about document architecture, and deleting the wrong one "
                    "destroys the authoritative copy. Generated artifacts "
                    "(AUTOMATED_AUDIT_REPORT*, superpowers/) and pointer-only paragraphs "
                    "are excluded; short paragraphs below min_words are too. Finds shared "
                    "WORDS, not shared meaning — a clean run is not proof of no "
                    "duplication. caller_cwd required.");
                docDedup["selection_hint"] = QStringLiteral(
                    "Use before a documentation review: every duplicated fact is re-read by "
                    "every lane on every loop and is a standing source of future findings, "
                    "so finding them once up front is cheaper than rediscovering them one "
                    "at a time.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd")};
                    QJsonObject props;
                    QJsonObject ddPath; ddPath["type"] = "string";
                        ddPath["description"] = QStringLiteral(
                            "Project-relative file or directory (a directory walks *.md "
                            "recursively). Default: the project's docs dir, else docs/.");
                    QJsonObject ddCwd; ddCwd["type"] = "string";
                        ddCwd["description"] = QStringLiteral(
                            "Your $PWD. Required — anchors the doc walk to your project.");
                    QJsonObject ddSim; ddSim["type"] = "number";
                        ddSim["description"] = QStringLiteral(
                            "Jaccard threshold, at or above which a pair reports (default "
                            "0.40, clamped to [0,1]). Measured on this corpus: 0.40 yields "
                            "275 pairs in 128 clusters and catches both known real pairs; "
                            "0.80 narrows to 112 pairs in 26 clusters. Raising it discards "
                            "true positives to make the report look tidy.");
                    QJsonObject ddWords; ddWords["type"] = "integer";
                        ddWords["description"] = QStringLiteral(
                            "Minimum words in a comparable paragraph (default 15, clamped "
                            "to [1,1000]). Keep it low — the real finds are 19-31 words, "
                            "so a value set to suppress noise deletes them.");
                    QJsonObject ddShingle; ddShingle["type"] = "integer";
                        ddShingle["description"] = QStringLiteral(
                            "Words per shingle (default 3, clamped to [2,10]). Larger is "
                            "stricter about word order, smaller pairs more freely.");
                    QJsonObject ddEtag; ddEtag["type"] = "string";
                        ddEtag["description"] = QStringLiteral(
                            "Server-issued etag from a prior call; an unchanged corpus "
                            "short-circuits to {ok:true, unchanged:true}.");
                    props["path"] = ddPath;
                    props["caller_cwd"] = ddCwd;
                    props["min_similarity"] = ddSim;
                    props["min_words"] = ddWords;
                    props["shingle_size"] = ddShingle;
                    props["etag_match"] = ddEtag;
                    schema["properties"] = props;
                    docDedup["inputSchema"] = schema;
                }
                tools.append(docDedup);

                // ANTS-3636 — doc_citations: resolve every path:line citation
                // in one doc and return the line it points at, so a reviewer
                // stops opening 33 files by hand to verify them.
                QJsonObject docCit;
                docCit["name"] = "doc_citations";
                docCit["description"] = QStringLiteral(
                    "Resolve a doc's `path:line` citations against the files and return the "
                    "cited line TEXT. Scans one markdown/text file, finds every src/a.cpp:12 "
                    "/ a.cpp:10-12 / `:45` continuation, resolves it (project-relative, then "
                    "the codebase-index basename map, then the repo root), and reports each "
                    "with status ok | missing_file | foreign_path | out_of_range | read_error "
                    "| ambiguous | unresolved plus the text. Fenced examples are skipped; a "
                    "citation inside an inline code span IS harvested. ANTS-4085 — "
                    "`foreign_path` is a path-shaped citation whose leading segment names no "
                    "directory here AND whose basename exists nowhere in the project: almost "
                    "always a QUOTATION of another repository's path, so it is reported and "
                    "counted but is NOT stale, because there is nothing here to fix and a "
                    "checker whose output is never empty stops being read. A path whose "
                    "basename the project DOES know stays `missing_file` — that is our file, "
                    "genuinely gone. only=\"stale\" narrows to the "
                    "non-ok ones; counts stay unfiltered. ANTS-4087 — `counts` mixes"
                    "STATUS buckets (which partition `count`) with OVERLAY buckets "
                    "layered on the ok subset, so the map does NOT sum to `count`; "
                    "`counts_overlay_keys` names the overlays, so assert "
                    "sum(counts[k] for k not in counts_overlay_keys) == count. "
                    "`unchecked` means resolved-but-nothing-to-cross-check (the doc "
                    "gave no anchor needle), NOT unverified — a clean pass over "
                    "anchorless citations reports every one of them there. offset pages; max_range_lines "
                    "bounds the lines per citation; max_bytes the response; max_doc_lines the "
                    "scanned prefix. Tokens that look like citations but resolve nowhere land "
                    "in unparsed[] rather than being guessed at. Read-only. caller_cwd "
                    "required.");
                docCit["selection_hint"] = QStringLiteral(
                    "Use when reviewing a doc full of file:line references, to check every "
                    "one still points where it claims — instead of opening each file by "
                    "hand.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    schema["required"] = QJsonArray{QStringLiteral("caller_cwd"),
                                                    QStringLiteral("path")};
                    QJsonObject props;
                    QJsonObject dcPath; dcPath["type"] = "string";
                        dcPath["description"] = QStringLiteral(
                            "Project-relative path to a single FILE to scan (a directory "
                            "refuses bad_args). Any UTF-8 text file, not only .md.");
                    QJsonObject dcOnly; dcOnly["type"] = "string";
                        dcOnly["enum"] = QJsonArray{QStringLiteral("all"),
                                                    QStringLiteral("stale")};
                        dcOnly["description"] = QStringLiteral(
                            "Filter over citations[] only: \"stale\" keeps the ones that did "
                            "not resolve cleanly. counts stay whole-doc. Default all.");
                    QJsonObject dcOffset; dcOffset["type"] = "integer";
                        dcOffset["description"] = QStringLiteral(
                            "0-based index into the post-filter list. next_offset in the "
                            "response resumes the page when a cap dropped entries.");
                    QJsonObject dcRange; dcRange["type"] = "integer";
                        dcRange["description"] = QStringLiteral(
                            "Max lines of text per citation (1-20, default 3). A longer cited "
                            "range sets range_truncated; end_line still echoes the doc.");
                    QJsonObject dcBytes; dcBytes["type"] = "integer";
                        dcBytes["description"] = QStringLiteral(
                            "Response byte budget (64 KiB - 4 MiB, default 128 KiB). A soft "
                            "ceiling: one entry always ships so paging cannot stall.");
                    QJsonObject dcDocLines; dcDocLines["type"] = "integer";
                        dcDocLines["description"] = QStringLiteral(
                            "Lines of the doc to scan (1000-50000, default 20000). "
                            "scanned_lines < doc_lines says the tail was not looked at.");
                    props["path"]            = dcPath;
                    props["only"]            = dcOnly;
                    props["offset"]          = dcOffset;
                    props["max_range_lines"] = dcRange;
                    props["max_bytes"]       = dcBytes;
                    props["max_doc_lines"]   = dcDocLines;
                    {   // ANTS-4386 — quotation check.
                        QJsonObject q; q["type"] = "boolean";
                                       q["default"] = false;
                                       q["description"] = QStringLiteral(
                            "Optional (ANTS-4386). Also check QUOTATIONS: a "
                            "double-quoted span of at least `quote_min_chars` "
                            "characters, resolved against the document "
                            "attributed to it by a backticked path on the same "
                            "line. Emits `quotes[]` "
                            "({line, text, target, status, candidates}) with "
                            "status ok | not_found | ambiguous | no_target, "
                            "plus `quote_counts` and `quotes_checked` — a "
                            "zero says the pass ran and found nothing to "
                            "check, not that every quotation is sound. "
                            "Matching folds runs of whitespace INCLUDING "
                            "NEWLINES on both sides: a quotation in a "
                            "hard-wrapped document does not survive a "
                            "line-oriented search, and reporting \"not "
                            "found\" for a phrase that is present is worse "
                            "than not checking — it makes a reviewer \"fix\" "
                            "a passage that was already correct. An "
                            "attribution naming a basename with several "
                            "matches is `ambiguous` WITH the hit list, never a "
                            "guess. A quotation inside a fence is a specimen "
                            "and is skipped. Off by default.");
                        props["quotes"] = q;
                        QJsonObject qm; qm["type"] = "integer";
                                        qm["default"] = 30;
                                        qm["minimum"] = 10;
                                        qm["maximum"] = 500;
                                        qm["description"] = QStringLiteral(
                            "Optional (ANTS-4386). Minimum length for a "
                            "double-quoted span to count as a quotation. "
                            "Keeps the check off ordinary quoted words.");
                        props["quote_min_chars"] = qm;
                    }
                    props["caller_cwd"]      = makeCallerCwdReadProp();
                    props["etag_match"]      = makeEtagMatchProp();   // ANTS-1499
                    props["encoding"]        = makeEncodingProp();    // ANTS-2090
                    schema["properties"] = props;
                    docCit["inputSchema"] = schema;
                }
                tools.append(docCit);

                // ANTS-2161 — project_settings: detect a misplaced layout +
                // create/update <root>/.ants/project.json so a non-src/
                // project (e.g. code under linuxdoom-1.10/) stops getting an
                // empty codebase_index. Companion to the read-side ANTS-2160.
                QJsonObject psTool;
                psTool["name"] = "project_settings";
                psTool["description"] = QStringLiteral(
                    "Detect a non-standard project layout and create/update the "
                    "repo-committed <root>/.ants/project.json (the ANTS-2160 "
                    "reader's source). op:\"detect\" (read-only) → {present, "
                    "suggestion:{source_roots?, reason, default_source_count, "
                    "total_source_count}} — suggests source_roots when the "
                    "default src/+tests/ walk would miss most of the repo's "
                    "code, and (ANTS-3705) echoes the CURRENT declaration as "
                    "`declared` plus `declared_missing[]` naming any declared "
                    "path that no longer resolves — so reading "
                    ".ants/project.json by hand is never needed. A missing "
                    "entry is DROPPED by the reader, which is otherwise "
                    "indistinguishable from never having been declared. "
                    "ANTS-4093 — detect suggests ALL SIX recognised keys, not "
                    "just source_roots: test_roots / docs_dir / specs_dir / "
                    "roadmap / changelog are proposed from their conventional "
                    "paths whenever those resolve, and `undeclared[]` names "
                    "every recognised key with no declaration. A reason of "
                    "\"no source_roots override needed\" is about source_roots "
                    "ALONE — it does not mean there is nothing to configure. "
                    "ANTS-4092 — gitignored directories are excluded from the "
                    "walk before it counts them (one `git check-ignore`, "
                    "matching workspace_search's respect_gitignore default) "
                    "and are named in `excluded[]` alongside the vendored / "
                    "build dirs; a repo-less or git-less root walks as before. "
                    "op:\"init\" → write the detected (or explicit) keys; "
                    "refuses settings_exists if the file is present (no "
                    "clobber); writes nothing (written:false) when there's "
                    "nothing to suggest. op:\"set\" → create-or-update any key "
                    "(merges into an existing file, preserving keys it doesn't "
                    "touch; a null value clears a key); refuses bad_args when no "
                    "key is supplied, unrecognised_format on a malformed "
                    "existing file. Declared paths are validated under the root "
                    "(bad_path) and the file is written world-readable. "
                    "caller_cwd required.");
                psTool["selection_hint"] = QStringLiteral(
                    "Use op:detect (or read session_orient's "
                    "project_settings_suggestion) when codebase_index comes back "
                    "near-empty; op:init to accept the suggestion, op:set to "
                    "declare source_roots/docs_dir/roadmap/etc. explicitly.");
                {
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject opProp; opProp["type"] = "string";
                        opProp["enum"] = QJsonArray{QStringLiteral("detect"),
                            QStringLiteral("init"), QStringLiteral("set")};
                        opProp["description"] = QStringLiteral(
                            "detect (preview, read-only) | init (create, no "
                            "clobber) | set (create-or-update). Required.");
                    const auto dirArrayProp = []() {
                        QJsonObject p; p["type"] = "array";
                        QJsonObject items; items["type"] = "string"; p["items"] = items;
                        return p;
                    };
                    QJsonObject srProp = dirArrayProp();
                        srProp["description"] = QStringLiteral(
                            "Source root dirs (repo-relative) for init/set; "
                            "replaces the src/ default. Must exist.");
                    QJsonObject trProp = dirArrayProp();
                        trProp["description"] = QStringLiteral(
                            "Test root dirs (repo-relative); replaces tests/.");
                    QJsonObject ddProp; ddProp["type"] = "string";
                        ddProp["description"] = QStringLiteral("Docs dir (repo-relative).");
                    QJsonObject sdProp; sdProp["type"] = "string";
                        sdProp["description"] = QStringLiteral("Specs dir (repo-relative).");
                    QJsonObject rmProp; rmProp["type"] = "string";
                        rmProp["description"] = QStringLiteral("Roadmap file (repo-relative).");
                    QJsonObject clProp; clProp["type"] = "string";
                        clProp["description"] = QStringLiteral("Changelog file (repo-relative).");
                    props["op"]           = opProp;
                    props["source_roots"] = srProp;
                    props["test_roots"]   = trProp;
                    props["docs_dir"]     = ddProp;
                    props["specs_dir"]    = sdProp;
                    props["roadmap"]      = rmProp;
                    props["changelog"]    = clProp;
                    props["dry_run"]      = makeDryRunProp();    // ANTS-2227
                    props["caller_cwd"]   = makeCallerCwdReadProp();
                    schema["properties"]  = props;
                    schema["required"]    = QJsonArray{QStringLiteral("op")};
                    psTool["inputSchema"] = schema;
                }
                tools.append(psTool);

#ifdef ANTS_LUA_PLUGINS
                // ANTS-2093 — project_query: run an agent-supplied READ-ONLY
                // Lua snippet server-side and return ONLY its result (the
                // code-execution token-saver). Listed only in ANTS_LUA_PLUGINS
                // builds — the verb is unregistered without the Lua subsystem.
                {
                    QJsonObject pqTool;
                    pqTool["name"] = "project_query";
                    pqTool["description"] = QStringLiteral(
                        "Run a small READ-ONLY Lua snippet over the project's "
                        "files server-side and get back ONLY its computed "
                        "result — not the file text. The token-saver for "
                        "\"compute an aggregate across files\" questions (count "
                        "TODOs, which files import X, sum sizes): the snippet "
                        "greps/filters/counts itself and you receive just the "
                        "number/list. API (the whole surface): project.read("
                        "relpath)->string, project.list(subdir?)->array of "
                        "project-relative file paths (sorted, skips .git/), "
                        "project.root()->string; plus the string/table/math/utf8 "
                        "stdlib. The snippet MUST `return` a value (the answer). "
                        "Sandboxed: read-only, confined to caller_cwd (no "
                        "escape, no write, no network, no os/io), 10 MiB + "
                        "wall-clock + output-size capped. NOT a workspace_search "
                        "replacement — scope to a known file set (or a "
                        "workspace_search result) then aggregate, don't pull the "
                        "whole tree through project.read. Success {ok, result, "
                        "elapsed_ms}; refusals query_error (bad/erroring snippet "
                        "or path escape), query_timeout, query_oom, "
                        "result_too_large (aggregate harder), query_disabled "
                        "(feature off). caller_cwd required.");
                    pqTool["selection_hint"] = QStringLiteral(
                        "Use when you'd otherwise Read several files just to "
                        "count/filter/aggregate over them — ship the count "
                        "logic as a Lua snippet and get back only the number.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject props;
                    QJsonObject codeProp;
                    codeProp["type"] = "string";
                    codeProp["description"] = QStringLiteral(
                        "The read-only Lua snippet. Must `return` a value. "
                        "Has project.read/list/root + string/table/math/utf8.");
                    props["code"]       = codeProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    schema["required"]   = QJsonArray{QStringLiteral("code"),
                                                      QStringLiteral("caller_cwd")};
                    pqTool["inputSchema"] = schema;
                    tools.append(pqTool);
                }
#endif

                // ANTS-1961 — feedback_query: read the un-triaged tail of
                // a *_Ants_MCP_Feedback.md file instead of a full Read.
                {
                    QJsonObject t;
                    t["name"] = "feedback_query";
                    t["description"] = QStringLiteral(
                        "Read only the UN-TRIAGED tail of a cross-session "
                        "*_Ants_MCP_Feedback.md file instead of Read-ing the "
                        "whole file. The un-triaged rule is VERSION-DEPENDENT "
                        "(ANTS-3448): on a v1 file the delta is everything a "
                        "contributor appended after the last maintainer tracking "
                        "block; on a v2 file (`<!-- ants-mcp-feedback: 2 -->` or "
                        "higher) it is the findings whose inline "
                        "`**Proposed ID:**` line is still unfilled (no id, no n/a "
                        "closure). Returns {ok, path, delta, delta_present, "
                        "delta_line_count, delta_start_line, mapped_ids, "
                        "mapped_id_status, format_version, suspected_untagged, "
                        "maintainer_block_count, last_maintainer_line, "
                        "truncated, etag}. `mapped_ids` are the assigned inline "
                        "`**Proposed ID:**` ids on a v2 file, or the ANTS-NNNN "
                        "ids cited in maintainer blocks on v1. `mapped_id_status` "
                        "(ANTS-3478; present only when mapped_ids is non-empty) "
                        "resolves each mapped id's LIVE status against the caller "
                        "project's ROADMAP.md — [{id, status, shipped_date?}] with "
                        "status one of 📋/🚧/✅/💭 or \"unknown\" (absent from the "
                        "live roadmap; may have archived — never silently ✅). "
                        "`shipped_date` (ANTS-3504) is the fix's ship-date (the id's "
                        "last ROADMAP `Resolved` date), present ONLY on ✅ ids that "
                        "have one — compare it against session_orient "
                        "server_build.build_date to tell a shipped fix from a "
                        "stale binary before re-reporting. `format_version` "
                        "is the detected marker version (0/1/2/…). "
                        "`suspected_untagged` (v2 only) lists {heading, line} for "
                        "`### ` finding-shaped blocks a hand editor left with no "
                        "`**Proposed ID:**` line (empty on v1 / a clean v2 file). "
                        "Under v2 `delta` is a concatenation of (possibly "
                        "non-contiguous) findings — treat it as opaque text, do "
                        "NOT re-slice the file from `delta_start_line`. "
                        "include_tracking:true (ANTS-3371) adds `tracking` "
                        "— every maintainer tracking-table row "
                        "[{item, ids, status, notes?}] in document order "
                        "(later rows supersede earlier ones for the same "
                        "id) — so a session sees which of its prior "
                        "suggestions shipped without hand-parsing the "
                        "tables. "
                        "Byte-capped (max_bytes, default 512 KiB, 4 MiB "
                        "ceiling) keeping the HEAD of the delta. `path` is "
                        "OPTIONAL (ANTS-3376): omit it and the conventional "
                        "<caller_cwd-leaf>_Ants_MCP_Feedback.md at the shared "
                        "root (the parent of caller_cwd) is derived — the "
                        "reply then carries path_derived:true. Refusals: "
                        "`bad_args` (no path AND no resolvable caller_cwd to "
                        "derive one), `not_feedback_file` (basename not "
                        "*_Ants_MCP_Feedback.md), `bad_path` (traversal), "
                        "`not_found` (an EXPLICIT `path` that does not "
                        "resolve). ANTS-4104 — a DERIVED default that does not "
                        "exist is NOT a refusal: \"nobody has filed anything "
                        "here yet\" is the state every project starts in, so it "
                        "returns ok:true with found:false, delta_present:false, "
                        "empty mapped_ids and a `reason`, keeping the "
                        "`candidates` + `hint` below. Every success carries "
                        "found:true, so one branch covers both. A `not_found` "
                        "envelope carries "
                        "`candidates` (sibling *_Ants_MCP_Feedback.md paths in "
                        "the same dir) + a `hint`; the caller's own file is "
                        "floated to the front, or `all_other_projects:true` "
                        "flags that every candidate belongs to a different "
                        "project (ANTS-3366/3376) — recover the right basename "
                        "without shelling out to `ls`. caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to pull just the new feedback from a shared "
                        "*_Ants_MCP_Feedback.md report file instead of "
                        "re-reading the whole thing each review.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional (ANTS-3376). Path to the "
                            "*_Ants_MCP_Feedback.md file. Absolute (the "
                            "canonical case — files live at "
                            "/mnt/Games/Scripts/Linux/) or caller_cwd-"
                            "relative. Omit to derive "
                            "<caller_cwd-leaf>_Ants_MCP_Feedback.md at the "
                            "shared root (parent of caller_cwd).");
                    QJsonObject mbProp; mbProp["type"] = "integer";
                        mbProp["minimum"] = 1;
                        mbProp["description"] = QStringLiteral(
                            "Cap on the emitted `delta` bytes (default "
                            "512 KiB, server-clamped to 4 MiB). Keeps the "
                            "HEAD; sets truncated:true. delta_line_count "
                            "still reports the full count.");
                    QJsonObject itProp; itProp["type"] = "boolean";
                        itProp["description"] = QStringLiteral(
                            "Optional (ANTS-3371). When true, add a "
                            "`tracking` array of every maintainer "
                            "tracking-table row [{item, ids, status, "
                            "notes?}] in document order. Use for the "
                            "\"which of my prior suggestions shipped?\" "
                            "workflow without a full-file Read.");
                    props["path"]            = pathProp;
                    props["max_bytes"]       = mbProp;
                    props["include_tracking"] = itProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"] = props;
                    // ANTS-3376 — `path` is now optional (derived from
                    // caller_cwd when omitted); only caller_cwd is required.
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1962 — feedback_log: append a contributor finding
                // block or a maintainer tracking block, rendered from
                // structured args (no hand-edited markdown).
                {
                    QJsonObject t;
                    t["name"] = "feedback_log";
                    t["description"] = QStringLiteral(
                        "Append to a cross-session "
                        "*_Ants_MCP_Feedback.md file — always at EOF, "
                        "never inserting above a maintainer block. "
                        "op:\"append_finding\" (contributor) renders a "
                        "dated session heading + one ### sub-block per "
                        "finding; op:\"append_tracking\" (maintainer) "
                        "renders the 📋 watermark heading + a mapping "
                        "table. Creates an absent file with a conforming "
                        "skeleton on append_finding; append_tracking on an "
                        "absent file refuses not_found (with `candidates` + "
                        "`hint` listing sibling *_Ants_MCP_Feedback.md "
                        "files — ANTS-3366). `path` is OPTIONAL (ANTS-3376): "
                        "omit it for a first-time log and the conventional "
                        "<caller_cwd-leaf>_Ants_MCP_Feedback.md at the shared "
                        "root is derived (reply carries path_derived:true). "
                        "Atomic write. Returns {ok, op, path, bytes_appended, "
                        "date, created}. Refusals: `bad_mode`, `bad_args` "
                        "(includes no path AND no resolvable caller_cwd), "
                        "`bad_status` (row status outside 📋🚧✅💭🔄❓), "
                        "`not_feedback_file`, `bad_path`, `not_found`, "
                        "`write_failed`. ANTS-3421: op:\"compact_shipped\" "
                        "(maintainer) collapses confirmed-shipped contributor "
                        "blocks named in `targets` to a one-line stub (heading "
                        "kept; gated ✅-shipped + above-watermark + "
                        "single-finding + idempotent; batch, atomic, dry_run "
                        "byte report; per-target refusals in skipped[]). "
                        "ANTS-3443: op:\"compact_resolved\" (maintainer, v2 "
                        "files) auto-collapses shipped findings' write-ups, "
                        "gated on the live ROADMAP.md status. ANTS-3446: "
                        "op:\"migrate_v2\" (maintainer) mechanically converts a "
                        "v1 file to v2 — bumps the version marker and stamps "
                        "blank \"**Proposed ID:**\" placeholders on un-triaged "
                        "findings; leaves the v1 tracking tables in place. "
                        "ANTS-3447: op:\"assign_id\" (maintainer, v2 triage) "
                        "fills ONE finding's \"**Proposed ID:**\" line in place "
                        "with the assigned ids or an n/a closure — the inline "
                        "replacement for append_tracking. caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to add feedback to a shared "
                        "*_Ants_MCP_Feedback.md file (or stamp a "
                        "maintainer tracking block) instead of "
                        "hand-editing markdown.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional (ANTS-3376). Path to the "
                            "*_Ants_MCP_Feedback.md file (absolute or "
                            "caller_cwd-relative). Omit to derive "
                            "<caller_cwd-leaf>_Ants_MCP_Feedback.md at the "
                            "shared root (parent of caller_cwd).");
                    QJsonObject opProp; opProp["type"] = "string";
                        { QJsonArray e; e.append(QStringLiteral("append_finding"));
                          e.append(QStringLiteral("append_tracking"));
                          e.append(QStringLiteral("compact_shipped"));
                          e.append(QStringLiteral("prune_tracking"));
                          e.append(QStringLiteral("compact_resolved"));
                          e.append(QStringLiteral("migrate_v2"));
                          e.append(QStringLiteral("assign_id"));
                          opProp["enum"] = e; }
                        opProp["description"] = QStringLiteral(
                            "Required. \"append_finding\" (contributor) "
                            "or \"append_tracking\" (maintainer). ANTS-3421 — "
                            "\"compact_shipped\" (maintainer) collapses each "
                            "confirmed-shipped contributor block named in "
                            "`targets` to a one-line \"→ shipped …\" stub "
                            "(heading kept verbatim; gated on the id being ✅ "
                            "in a tracking row + above the watermark; dry_run "
                            "previews the byte savings). ANTS-3442 — "
                            "\"prune_tracking\" (maintainer) removes superseded "
                            "DUPLICATE maintainer tracking-table rows, keeping "
                            "each id's authoritative last row + every heading/"
                            "header/separator (dedup of the repeated rows an id "
                            "accrues 📋→🚧→✅ across tables). Optional "
                            "`scope_ids` restricts to given ids; `dry_run` "
                            "previews `rows_removed`/`bytes_saved`. Idempotent; "
                            "atomic. ANTS-3443 — \"compact_resolved\" "
                            "(maintainer, v2 files only) auto-collapses every "
                            "shipped finding's write-up to a \"→ shipped ✅ "
                            "(write-up compacted, ANTS-3443)\" stub that keeps "
                            "the heading + its `**Proposed ID:**` line; gated "
                            "per-finding on the id(s) being ✅ in the live "
                            "ROADMAP.md (resolved from caller_cwd). No targets — "
                            "auto-discovery; `dry_run` previews "
                            "`findings_collapsed`/`bytes_saved`; per-finding "
                            "skips (no_shippable_id / already_compacted / "
                            "roadmap_unresolved_ids / has_open_id) in skipped[]. "
                            "Idempotent; atomic. Refuses `not_v2` on a v1 file "
                            "(run migrate_v2 first), `roadmap_unavailable` when "
                            "ROADMAP.md is unreadable. ANTS-3446 — "
                            "\"migrate_v2\" (maintainer) one-shot mechanical "
                            "v1→v2 conversion: bumps the version marker to "
                            "\"<!-- ants-mcp-feedback: 2 -->\" and stamps a "
                            "blank \"- **Proposed ID:** _(maintainer to "
                            "assign)_\" line on each finding-shaped, "
                            "below-watermark `### ` block that lacks one; the "
                            "v1 tracking tables are LEFT IN PLACE (not moved/"
                            "collapsed). No roadmap read. Reports "
                            "`stamped`/`orphans`/`unclassified`; `already_v2` "
                            "true (byte-identical no-op) on a v2 file; `dry_run` "
                            "previews. Idempotent; atomic. ANTS-3474 — pass "
                            "`backfill_from_tracking:true` to carry each "
                            "finding's id in from the file's own v1 tracking "
                            "tables (confidence-gated; `backfilled[]` in the "
                            "reply, blank on any uncertainty) instead of a blank "
                            "stamp. ANTS-3447 — "
                            "\"assign_id\" (maintainer, v2 triage) fills ONE "
                            "`### ` finding's `**Proposed ID:**` line: pass "
                            "`heading` (the verbatim `### ` line, + optional "
                            "`heading_line` to disambiguate a repeat) and "
                            "exactly one of `ids` (an ANTS-NNNN array → "
                            "comma-joined) OR `closure` (a reason string → "
                            "`n/a — <reason>`, or bare `n/a` when empty). "
                            "Replaces the existing / placeholder id line, or "
                            "inserts one when absent. No roadmap read. Reports "
                            "`value`/`inserted`/`changed`/`bytes_delta`; "
                            "idempotent (a re-assign of the same value is a "
                            "byte-identical no-op); `dry_run` previews. Refuses "
                            "`target_not_found` / `target_ambiguous` (+ "
                            "`candidates`).");
                    QJsonObject dateProp; dateProp["type"] = "string";
                        dateProp["description"] = QStringLiteral(
                            "Optional YYYY-MM-DD; defaults to today.");
                    QJsonObject labelProp; labelProp["type"] = "string";
                        labelProp["description"] = QStringLiteral(
                            "Optional session label (append_finding "
                            "heading suffix).");
                    QJsonObject hlProp; hlProp["type"] = "string";
                        { QJsonArray e; e.append(QStringLiteral("h1"));
                          e.append(QStringLiteral("h2")); hlProp["enum"] = e; }
                        hlProp["description"] = QStringLiteral(
                            "Optional finding heading level, default h2.");
                    QJsonObject noteProp; noteProp["type"] = "string";
                        noteProp["description"] = QStringLiteral(
                            "Optional prose under the heading (append_finding / "
                            "append_tracking). ANTS-3571 — on assign_id it is "
                            "written as a single `- **Note:**` bullet under the "
                            "finding's Proposed ID line (newlines folded to "
                            "spaces; re-assign replaces it in place).");
                    QJsonObject findingsProp; findingsProp["type"] = "array";
                        { QJsonObject it; it["type"] = "object";
                          QJsonObject fp;
                          QJsonObject s; s["type"] = "string";
                          fp["title"] = s; fp["what"] = s; fp["repro"] = s;
                          fp["impact"] = s; fp["suggested_fix"] = s;
                          it["properties"] = fp;
                          findingsProp["items"] = it; }
                        findingsProp["description"] = QStringLiteral(
                            "append_finding: ≥1 finding. Each: title "
                            "(required) + optional what/repro/impact/"
                            "suggested_fix.");
                    QJsonObject rowsProp; rowsProp["type"] = "array";
                        { QJsonObject it; it["type"] = "object";
                          QJsonObject rp;
                          QJsonObject s; s["type"] = "string";
                          QJsonObject idsArr; idsArr["type"] = "array";
                          QJsonObject idIt; idIt["type"] = "string";
                          idsArr["items"] = idIt;
                          rp["item"] = s; rp["ids"] = idsArr;
                          rp["status"] = s; rp["notes"] = s;
                          it["properties"] = rp;
                          rowsProp["items"] = it; }
                        rowsProp["description"] = QStringLiteral(
                            "append_tracking: ≥1 row. Each: item "
                            "(required), ids (ANTS-NNNN array, may be "
                            "empty → n/a), status (📋🚧✅💭🔄❓), notes "
                            "(optional).");
                    QJsonObject sentinelProp; sentinelProp["type"] = "boolean";
                        sentinelProp["description"] = QStringLiteral(
                            "append_tracking: emit the trailing \"End of "
                            "…\" breadcrumb (default true).");
                    QJsonObject targetsProp; targetsProp["type"] = "array";
                        { QJsonObject it; it["type"] = "object";
                          QJsonObject tp;
                          QJsonObject s; s["type"] = "string";
                          QJsonObject i; i["type"] = "integer";
                          tp["heading"] = s; tp["heading_line"] = i;
                          tp["id"] = s; tp["session"] = s; tp["date"] = s;
                          it["properties"] = tp;
                          targetsProp["items"] = it; }
                        targetsProp["description"] = QStringLiteral(
                            "compact_shipped: ≥1 target block to collapse. "
                            "Each: heading (required — the block's verbatim "
                            "`#`/`## ` heading line), id (required — the "
                            "ANTS-NNNN it shipped as, gated ✅), optional "
                            "heading_line (1-based, disambiguates a repeated "
                            "heading), optional session/date for the stub "
                            "breadcrumb.");
                    props["path"]          = pathProp;
                    props["op"]            = opProp;
                    props["date"]          = dateProp;
                    props["session_label"] = labelProp;
                    props["heading_level"] = hlProp;
                    props["note"]          = noteProp;
                    props["findings"]      = findingsProp;
                    props["rows"]          = rowsProp;
                    props["sentinel"]      = sentinelProp;
                    props["targets"]       = targetsProp;        // ANTS-3421
                    QJsonObject scopeIdsProp; scopeIdsProp["type"] = "array";
                        { QJsonObject idIt; idIt["type"] = "string";
                          scopeIdsProp["items"] = idIt; }
                        scopeIdsProp["description"] = QStringLiteral(
                            "prune_tracking (ANTS-3442): optional ANTS-NNNN ids "
                            "to restrict pruning to. OMIT to prune every "
                            "superseded row; an explicitly EMPTY array is "
                            "bad_args (restrict-to-nothing is a no-op you almost "
                            "never mean).");
                    props["scope_ids"]     = scopeIdsProp;        // ANTS-3442
                    // ANTS-3447 — assign_id top-level fields.
                    QJsonObject headingProp; headingProp["type"] = "string";
                        headingProp["description"] = QStringLiteral(
                            "assign_id (ANTS-3447): the target finding's verbatim "
                            "`### ` heading line, including the `### ` prefix "
                            "(trimmed-matched against the finding headings).");
                    QJsonObject headingLineProp; headingLineProp["type"] = "integer";
                        headingLineProp["description"] = QStringLiteral(
                            "assign_id: optional 1-based line of the `### ` "
                            "heading — disambiguates a repeated heading "
                            "(target_ambiguous otherwise).");
                    QJsonObject idsProp; idsProp["type"] = "array";
                        { QJsonObject idIt; idIt["type"] = "string";
                          idsProp["items"] = idIt; }
                        idsProp["description"] = QStringLiteral(
                            "assign_id: the ANTS-NNNN ids to assign (each "
                            "^ANTS-[0-9]+$; rendered comma-joined, de-duplicated). "
                            "Supply EITHER ids OR closure, never both.");
                    QJsonObject closureProp; closureProp["type"] = "string";
                        closureProp["description"] = QStringLiteral(
                            "assign_id: a closure reason → writes "
                            "`n/a — <reason>` (empty string → bare `n/a`). "
                            "Supply EITHER ids OR closure, never both.");
                    props["heading"]       = headingProp;         // ANTS-3447
                    props["heading_line"]  = headingLineProp;     // ANTS-3447
                    props["ids"]           = idsProp;             // ANTS-3447
                    props["closure"]       = closureProp;         // ANTS-3447
                    // ANTS-3474 — migrate_v2 tracking-table backfill opt-in.
                    QJsonObject backfillProp; backfillProp["type"] = "boolean";
                        backfillProp["default"] = false;
                        backfillProp["description"] = QStringLiteral(
                            "migrate_v2 (ANTS-3474): when true, backfill each "
                            "migrated finding's inline `**Proposed ID:**` from "
                            "the file's own v1 tracking rows (token-match "
                            "heading↔item; a single id clearing the confidence "
                            "threshold with a clear margin is stamped inline, "
                            "else left blank — never a wrong id). The reply's "
                            "`backfilled[]` lists each {heading, line, id, "
                            "confidence_pct}; `dry_run` previews them for review. "
                            "Default false = the mechanical blank-stamp migrate.");
                    props["backfill_from_tracking"] = backfillProp;  // ANTS-3474
                    props["dry_run"]       = makeDryRunProp();   // ANTS-2227
                    props["caller_cwd"]    = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    // ANTS-3376 — `path` is now optional (derived from
                    // caller_cwd when omitted); op + caller_cwd required.
                    QJsonArray req;
                    req.append(QStringLiteral("op"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-2129 — audit_falsepos_log: append a confirmed
                // false positive to the prose review ledger
                // .ants_review_falsepos.jsonl (write side of ANTS-1457).
                {
                    QJsonObject t;
                    t["name"] = "audit_falsepos_log";
                    t["description"] = QStringLiteral(
                        "Append one confirmed false-positive record to "
                        "<project>/.ants_review_falsepos.jsonl — the prose "
                        "ledger the /cold-eyes, /indie-review, /test-audit, "
                        "/debt-sweep (and /audit step-10.5) sweeps read so a re-run "
                        "doesn't re-litigate a dismissed finding. Atomic "
                        "O_APPEND (safe under concurrent CC sessions — do "
                        "NOT hand-write with the Write tool). Trims claim/"
                        "rationale to the read caps and refuses a record "
                        "over 3.5 KiB. Creates an absent ledger (mode 0644). "
                        "ANTS-4105 — this ledger is READ BY THE AI-REVIEW "
                        "BRIEFS and does NOT suppress anything in audit_run: "
                        "the engine's suppressions:\"auto\" filters on the "
                        "FINGERPRINT ledger (.audit_cache/learned-fp.jsonl), "
                        "which `audit_dismiss` writes. So a static-analysis "
                        "TOOL finding goes to audit_dismiss; a reviewer's "
                        "prose claim goes here. The two are deliberately not "
                        "merged — their keys differ (a prose claim vs a "
                        "file+rule+message hash). The reply repeats this as "
                        "`consumed_by` + `hint`. "
                        "Returns {ok, path, bytes_appended, created, "
                        "timestamp, review_kind, consumed_by, hint}. "
                        "Refusals: `bad_args` "
                        "(empty claim/rationale, non-canonical review_kind, "
                        "bad timestamp, or over-size record), `no_project` "
                        "(caller_cwd unresolved), `write_failed` (I/O error "
                        "or non-regular ledger path). caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        // Kept under the ANTS-1453 240-char hint budget; the
                        // full ANTS-4105 routing lives in the description.
                        "Use after a sweep fold-in classifies a finding "
                        "FALSE_POSITIVE (user-confirmed). NOT for a "
                        "static-analysis tool finding — that goes to "
                        "audit_dismiss; this ledger feeds reviewer briefs and "
                        "suppresses nothing in audit_run.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject rkProp; rkProp["type"] = "string";
                        { QJsonArray e;
                          e.append(QStringLiteral("audit"));
                          e.append(QStringLiteral("cold-eyes"));
                          e.append(QStringLiteral("indie-review"));
                          e.append(QStringLiteral("test-audit"));
                          e.append(QStringLiteral("debt-sweep"));  // ANTS-3701
                          rkProp["enum"] = e; }
                        rkProp["description"] = QStringLiteral(
                            "Required. Which sweep dismissed it.");
                    QJsonObject claimProp; claimProp["type"] = "string";
                        claimProp["description"] = QStringLiteral(
                            "Required. One-line summary of the dismissed "
                            "claim (trimmed to 280 units).");
                    QJsonObject ratProp; ratProp["type"] = "string";
                        ratProp["description"] = QStringLiteral(
                            "Required. Why it's a false positive — cite the "
                            "file/line/system that makes it safe (trimmed to "
                            "1024 units). No secrets; flows into the brief.");
                    QJsonObject tsProp; tsProp["type"] = "string";
                        tsProp["description"] = QStringLiteral(
                            "Optional YYYY-MM-DD; defaults to today.");
                    QJsonObject laneProp; laneProp["type"] = "string";
                        laneProp["description"] = QStringLiteral(
                            "Optional lane/partition the finding belongs to "
                            "(empty = all lanes).");
                    QJsonObject topicProp; topicProp["type"] = "string";
                        topicProp["description"] = QStringLiteral(
                            "Optional short grouping tag "
                            "(e.g. rate-limit, unused-import).");
                    QJsonObject lbProp; lbProp["type"] = "string";
                        lbProp["description"] = QStringLiteral(
                            "Optional audit-trail tag "
                            "(user-confirmed / cc-session / external); "
                            "defaults to cc-session.");
                    props["review_kind"] = rkProp;
                    props["claim"]       = claimProp;
                    props["rationale"]   = ratProp;
                    props["timestamp"]   = tsProp;
                    props["lane"]        = laneProp;
                    props["topic"]       = topicProp;
                    props["logged_by"]   = lbProp;
                    props["dry_run"]     = makeDryRunProp();     // ANTS-2227
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("review_kind"));
                    req.append(QStringLiteral("claim"));
                    req.append(QStringLiteral("rationale"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1713 — audit_dismiss: record a learned-false-positive
                // verdict into the fingerprint ledger both audit_run and the
                // Audit dialog filter on. Deferred v2 of ANTS-1708.
                {
                    QJsonObject t;
                    t["name"] = "audit_dismiss";
                    t["description"] = QStringLiteral(
                        "Record one learned-false-positive verdict into "
                        "<project>/.audit_cache/learned-fp.jsonl — the "
                        "fingerprint-keyed ledger BOTH `audit_run` and the "
                        "Audit dialog filter on, so a dismissed finding stays "
                        "suppressed on every later sweep (and survives edits "
                        "that shift its line: the fingerprint is "
                        "line-independent). Pass `rule` plus EITHER "
                        "`fingerprint` OR `file`+`message` (preferred — the "
                        "server hashes them the same way the engine will look "
                        "them up). Re-dismissing an already-recorded "
                        "fingerprint is a no-op, not an error. Returns {ok, "
                        "path, fingerprint, computed, rule, timestamp}. "
                        "Refusals: `bad_args` (missing rule, or neither "
                        "fingerprint nor file+message, or a malformed "
                        "fingerprint), `no_project` (caller_cwd unresolved), "
                        "`write_failed` (I/O error). caller_cwd required. "
                        "NOT the same as audit_falsepos_log, which writes the "
                        "prose ledger the review skills read.");
                    t["selection_hint"] = QStringLiteral(
                        "Use when you have verified an audit finding is a "
                        "false positive and want future audits to stop "
                        "reporting it — previously only the Audit dialog "
                        "could teach the ledger.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject ruleProp; ruleProp["type"] = "string";
                        ruleProp["description"] = QStringLiteral(
                            "Required. The audit check id the finding came "
                            "from. For the line-based tools (cppcheck, clazy, "
                            "clang-tidy, mypy, shellcheck) this is the tool "
                            "name; JSON tools use their own check id.");
                    QJsonObject fileProp; fileProp["type"] = "string";
                        fileProp["description"] = QStringLiteral(
                            "The finding's file, as the tool spelled it. "
                            "Required unless `fingerprint` is given.");
                    QJsonObject msgProp; msgProp["type"] = "string";
                        msgProp["description"] = QStringLiteral(
                            "The finding's message. A leading "
                            "<path>:<line>[:<col>]: prefix is stripped before "
                            "hashing, so the verdict survives line shifts. "
                            "Required unless `fingerprint` is given.");
                    QJsonObject fpProp; fpProp["type"] = "string";
                        fpProp["description"] = QStringLiteral(
                            "Optional 16-lowercase-hex fingerprint, when you "
                            "already have it. Prefer file+message.");
                    QJsonObject reasonProp; reasonProp["type"] = "string";
                        reasonProp["description"] = QStringLiteral(
                            "Optional human note — why it isn't a real bug. "
                            "Recorded for the next reader; no secrets.");
                    props["rule"]        = ruleProp;
                    props["file"]        = fileProp;
                    props["message"]     = msgProp;
                    props["fingerprint"] = fpProp;
                    props["reason"]      = reasonProp;
                    props["dry_run"]     = makeDryRunProp();
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("rule"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

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
                    "`git diff`. op (\"status\" / \"log\" / \"diff\") "
                    "defaults to \"status\" when omitted (ANTS-3365 — a "
                    "bare git_state{caller_cwd} returns the one-call "
                    "status+branch+ahead/behind read). Op-specific: n "
                    "(log only, default 10, "
                    "cap 100), path (log/diff filter), range (diff "
                    "only, e.g. HEAD~5..HEAD; omit for the working-tree "
                    "diff — ANTS-2074), body (log only, "
                    "include commit body). Saves ~14-300 tokens per "
                    "call vs Bash. op=\"status\" envelope (ANTS-1522): "
                    "`files[]` now includes untracked paths with "
                    "`index:\"?\"` + `worktree:\"?\"` for `git status "
                    "--porcelain` parity — one array, one shape. "
                    "`untracked[]` (DEPRECATED) is still emitted in "
                    "parallel for one release; removed in 0.7.93. "
                    "op=\"diff\" (ANTS-3377): hunks=true emits per-file @@ "
                    "hunk headers {path, hunks:[{header, old_start, "
                    "old_count, new_start, new_count, lines?}]} for a clean "
                    "commit split (include_lines attaches hunk bodies, "
                    "context sets the -U width); staged=true diffs the index "
                    "vs HEAD.");
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
                    opProp["default"] = "status";   // ANTS-3365
                    opProp["description"] = QStringLiteral(
                        "defaults to \"status\" when omitted");
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
                        QStringLiteral("diff: e.g. HEAD~5..HEAD. Omit for "
                                       "the working-tree diff (unstaged "
                                       "changes, like bare `git diff`) — "
                                       "ANTS-2074.");
                    QJsonObject bodyProp;   bodyProp["type"]  = "boolean";
                                            bodyProp["default"] = false;
                                            bodyProp["description"] =
                        QStringLiteral("log: include commit body");
                    // ANTS-3377 — diff hunk-header mode + staged/index diff.
                    QJsonObject hunksProp;  hunksProp["type"] = "boolean";
                                            hunksProp["default"] = false;
                                            hunksProp["description"] =
                        QStringLiteral("diff: emit per-file @@ hunk headers "
                                       "{path, hunks:[{header, old_start, "
                                       "old_count, new_start, new_count, "
                                       "lines?}]} for a clean commit split, "
                                       "instead of the --numstat line counts");
                    QJsonObject stagedProp; stagedProp["type"] = "boolean";
                                            stagedProp["default"] = false;
                                            stagedProp["description"] =
                        QStringLiteral("diff: diff the index vs HEAD "
                                       "(git diff --cached); mutually "
                                       "exclusive with range");
                    QJsonObject incLProp;   incLProp["type"] = "boolean";
                                            incLProp["default"] = false;
                                            incLProp["description"] =
                        QStringLiteral("diff+hunks: attach each hunk's raw "
                                       "body lines");
                    QJsonObject ctxProp;    ctxProp["type"] = "integer";
                                            ctxProp["default"] = 3;
                                            ctxProp["minimum"] = 0;
                                            ctxProp["maximum"] = 10;
                                            ctxProp["description"] =
                        QStringLiteral("diff+hunks: unified-context width");
                    props["op"]    = opProp;
                    props["n"]     = nProp;
                    props["path"]  = pathProp;
                    props["range"] = rangeProp;
                    props["body"]  = bodyProp;
                    props["hunks"]         = hunksProp;
                    props["staged"]        = stagedProp;
                    props["include_lines"] = incLProp;
                    props["context"]       = ctxProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
                    schema["properties"] = props;
                    // ANTS-3365: op no longer required — defaults to
                    // "status". caller_cwd is enforced by the dispatcher's
                    // CallerCwdContract (Required), not the JSON schema.
                    schema["required"] = QJsonArray();
                    gsTool["inputSchema"] = schema;
                }
                tools.append(gsTool);

                // ANTS-1251: subsystem — single tool, dispatches on
                // `op` (map / files / recent_changes). Pre-parses the
                // module map (docs/subsystems.md, else CLAUDE.md —
                // ANTS-1292) and serves per-lane chunks so /indie-review
                // reviewers don't each re-read the file.
                QJsonObject ssTool;
                ssTool["name"] = "subsystem";
                ssTool["description"] = QStringLiteral(
                    "Query the project's subsystem (lane) map parsed "
                    "from docs/subsystems.md (falling back to CLAUDE.md "
                    "when absent — ANTS-1292). Three ops: map (all lanes), "
                    "files (per-lane file list), recent_changes "
                    "(per-lane git log). Required: op. lane required "
                    "for files / recent_changes. n: recent_changes "
                    "only (default 10, cap 100). name: map only — "
                    "case-insensitive substring filter over lane names "
                    "(ANTS-3414). Saves ~24 K tokens per /indie-review run.");
                ssTool["selection_hint"] = QStringLiteral(
                    "Use as the first call on a 'where does feature "
                    "X live?' question — walks the docs/subsystems.md "
                    "module map. Collapses 3-5 grep rounds into one.");
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
                    // ANTS-3414 — op:map optional `name` substring filter.
                    // Narrows the returned lane list to lanes whose name
                    // contains this needle (case-insensitive); empty/missing
                    // → the full map. Declared here so it is a real param
                    // (no longer flagged in ignored_args) — Vestige feedback.
                    QJsonObject nameProp;   nameProp["type"] = "string";
                                            nameProp["description"] =
                        QStringLiteral("op:map only — return only lanes whose "
                                       "name contains this substring "
                                       "(case-insensitive); omit for all lanes");
                    props["op"]   = opProp;
                    props["lane"] = laneProp;
                    props["n"]    = nProp;
                    props["name"] = nameProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();   // ANTS-1499
                    props["fields"] = makeFieldsProp();          // ANTS-1720
                    props["compact"] = makeCompactProp();        // ANTS-2091
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
                    "--check-level=exhaustive for full coverage. "
                    "ANTS-1406: pass `since_commit:<sha>` to short-"
                    "circuit redundant /audit re-runs — the response "
                    "carries `fresh:false` when the cached summary "
                    "is at a different commit or older than 5 min, "
                    "letting /close-phase skip a no-op audit "
                    "dispatch.");
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
                    // ANTS-1406 — `since_commit` short-circuit. When
                    // set, returns the cached snapshot only if it was
                    // produced at-or-after the supplied SHA (exact-match
                    // semantics) AND within a 5-minute freshness window.
                    // Otherwise returns {ok:true, fresh:false,
                    // since_commit, last_run_commit?, last_run_age_ms,
                    // reason:"commit_drift" | "stale_mtime" |
                    // "no_provenance"}. /close-phase uses this to skip
                    // a redundant /audit re-run when the prior pass is
                    // already cached at HEAD.
                    QJsonObject sinceCommitProp;
                    sinceCommitProp["type"] = "string";
                    sinceCommitProp["description"] = QStringLiteral(
                        "Optional. Short-circuit gate (ANTS-1406). "
                        "Pass your current HEAD SHA (full or shortened "
                        "≥7 hex chars). Server returns the cached "
                        "summary only if it was produced at this exact "
                        "commit AND the report mtime is within 5 min. "
                        "Otherwise responds {ok:true, fresh:false, "
                        "since_commit, last_run_commit?, "
                        "last_run_age_ms, reason} so callers can "
                        "decide whether to run a fresh audit. Saves "
                        "the ~45-50 K tokens /close-phase otherwise "
                        "burns re-running gates already known clean.");
                    props["top_n"]          = topNProp;
                    props["severity_floor"] = floorProp;
                    props["rule_ids"]       = ruleIdsProp;
                    props["since_commit"]   = sinceCommitProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"]     = makeCallerCwdReadProp();
                    props["etag_match"]     = makeEtagMatchProp();   // ANTS-1499
                    schema["properties"]    = props;
                    lasTool["inputSchema"] = schema;
                }
                tools.append(lasTool);

                // ANTS-1569 — current_state aggregator. Bundles
                // roadmap_query (active filter) + git_state(status) +
                // last_audit_summary + .claude/workflow.md parse +
                // docs/specs/<id>.md probe into one envelope. The
                // session-start dance is currently a 3-4 read cascade;
                // this verb collapses it.
                {
                    QJsonObject csTool;
                    csTool["name"] = "current_state";
                    csTool["description"] = QStringLiteral(
                        "One-call session-start state. Returns "
                        "{ok, active_bullet?, workflow_status_line?, "
                        "git_branch_state, open_audit_findings_count, "
                        "open_audit_findings_count_stale, spec_path?, "
                        "etag}. open_audit_findings_count_stale (ANTS-3370) "
                        "mirrors last_audit_summary's stale signal — true "
                        "means the cached audit predates HEAD, so the count "
                        "may describe already-fixed findings; discount it "
                        "and re-run audit_run. Bundles roadmap_query "
                        "(status:active) + git_state(op:status) + "
                        "last_audit_summary + .claude/workflow.md "
                        "best-effort parse + docs/specs/<active-id>.md "
                        "existence probe. `active_bullet` is the first "
                        "🚧 in document order, else the first 📋; "
                        "omitted when neither exists. Upstream "
                        "failures collapse to documented field "
                        "fallbacks — `ok:true` is preserved as long "
                        "as the project root resolves. Etag-able via "
                        "the ANTS-1499 304 pattern: pass `etag_match` "
                        "to short-circuit when nothing has changed. "
                        "See docs/specs/ANTS-1569.md.");
                    csTool["selection_hint"] = QStringLiteral(
                        "Use once at session start instead of chaining "
                        "roadmap_query + git_state + last_audit_summary "
                        "— one round-trip, ~3 KB envelope. Pass the "
                        "returned `etag` as `etag_match` on subsequent "
                        "calls within the session for 304 short-circuit.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    csTool["inputSchema"] = schema;
                    tools.append(csTool);
                }

                // ANTS-1724 — session_brief: compact session-state envelope.
                {
                    QJsonObject t;
                    t["name"] = "session_brief";
                    t["selection_hint"] = QStringLiteral(
                        "Use to orient a fresh /clear session: returns "
                        "git branch, build/test result, open audit "
                        "count, and active roadmap item in one call.");
                    t["description"] = QStringLiteral(
                        "Compact session-state envelope for orienting "
                        "a fresh session in one call. Returns git "
                        "branch+ahead/behind+files_changed_count, last "
                        "build result (pass/fail/unknown) with "
                        "error/warning counts, last test result with "
                        "pass/fail/total counts, open audit findings "
                        "count, and the active roadmap item id+headline. "
                        "All data comes from on-disk caches — no new I/O. "
                        "ETag-eligible: pass etag_match from a prior call "
                        "to skip re-emission when state is unchanged. "
                        "ANTS-1724.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject cwdProp;
                    cwdProp["type"] = "string";
                    cwdProp["description"] = QStringLiteral("Your $PWD (required).");
                    QJsonObject etagProp;
                    etagProp["type"] = "string";
                    etagProp["description"] = QStringLiteral(
                        "ETag from a previous session_brief call. "
                        "When it matches: {ok:true,unchanged:true,etag:\"<same>\"}.");
                    QJsonObject props;
                    props["caller_cwd"] = cwdProp;
                    props["etag_match"] = etagProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1883 — session_orient: bundle of current_state +
                // project_layout + roadmap_query (section_index, active).
                {
                    QJsonObject t;
                    t["name"] = "session_orient";
                    t["selection_hint"] = QStringLiteral(
                        "Use as the first read on a fresh /clear "
                        "session: bundles current_state + project_layout "
                        "+ roadmap_query section_index in one call "
                        "(one ETag).");
                    t["description"] = QStringLiteral(
                        "Single-call session-orientation bundle: composes "
                        "current_state (project / git / audit state) + "
                        "project_layout (where docs / specs / roadmap "
                        "live) + roadmap_query mode:section_index "
                        "status:\"active\" (active roadmap sections) + "
                        "active_bullets (top-20 active item headlines — "
                        "ANTS-4399: carries an `active_bullets_hint` when that "
                        "slice is a minority of the queue, because the list is "
                        "ordered by POSITION IN THE FILE and not by priority; "
                        "do not plan from it alone, use roadmap_query "
                        "mode:\"bundles\" instead; "
                        "mode:headline_only — resolve the next work "
                        "bundle without a follow-up call) + server_build "
                        "(ANTS-2073 — the running server's version + git "
                        "SHA + build date, so a client self-diagnoses a "
                        "stale-binary deploy gap) + codebase_index "
                        "(ANTS-2140 — a trimmed codebase-map summary; the "
                        "call eagerly refreshes the index per ANTS-1637 so "
                        "it is fresh at session start; volatile "
                        "generated_at_ms / refreshed_files are stripped to "
                        "keep the bundle ETag stable) "
                        "into one envelope under a single ETag. Use as "
                        "the first read on a fresh /clear session — "
                        "saves four MCP round-trips and four ETag "
                        "misses vs the per-verb-call orientation "
                        "pattern. Top-level ok is true IFF the first "
                        "three upstreams succeeded (active_bullets "
                        "does not affect ok — absent roadmap is not "
                        "a failure); on any upstream failure "
                        "the failing key carries that upstream's "
                        "verbatim refusal envelope. ANTS-1883. "
                        "ANTS-3587: an upstream that refuses ONLY because "
                        "an optional artifact is absent (no ROADMAP.md yet — "
                        "no_roadmap_loaded) keeps ok:true and is surfaced in "
                        "a notices[] array, so a fresh project's first call "
                        "does not read as a failure. "
                        "Etag tip: cache the returned `etag` field and "
                        "pass it back via `etag_match` on subsequent "
                        "calls in the same session — saves a full "
                        "re-emit when the underlying file hasn't "
                        "changed (ANTS-1499 \"304 Not Modified\" pattern).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject cwdProp;
                    cwdProp["type"] = "string";
                    cwdProp["description"] = QStringLiteral(
                        "Your $PWD (required).");
                    QJsonObject etagProp;
                    etagProp["type"] = "string";
                    etagProp["description"] = QStringLiteral(
                        "ETag from a previous session_orient call. "
                        "When it matches: "
                        "{ok:true,unchanged:true,etag:\"<same>\"}.");
                    QJsonObject props;
                    props["caller_cwd"] = cwdProp;
                    props["etag_match"] = etagProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1723 — workflow_state: superpowers skill step/phase store.
                {
                    QJsonObject t;
                    t["name"] = "workflow_state";
                    t["selection_hint"] = QStringLiteral(
                        "Use to persist skill step/phase across /clear. "
                        "Call op:\"get\" at session start to resume.");
                    t["description"] = QStringLiteral(
                        "Per-project, per-skill step/phase store for "
                        "superpowers skills. Survives /clear — call "
                        "op:\"get\" at session start to resume from the "
                        "last saved step. ops: get (returns {found,state?}), "
                        "set (stores {step,phase,notes}), clear (deletes). "
                        "Entries expire after 72 h of inactivity. "
                        "skill: ^[A-Za-z0-9_-]{1,32}$ "
                        "(e.g. \"tdd\", \"systematic-debugging\"). "
                        "Keys stored as wf.<skill> in session_memory "
                        "backing store. Write ops (set/clear) require "
                        "focused-tab cwd match (ANTS-1435). "
                        "ANTS-1723.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    schema["additionalProperties"] = false;
                    QJsonObject opProp;
                    opProp["type"] = "string";
                    QJsonArray opEnum;
                    opEnum.append(QStringLiteral("get"));
                    opEnum.append(QStringLiteral("set"));
                    opEnum.append(QStringLiteral("clear"));
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral("get/set/clear.");
                    QJsonObject cwdProp;
                    cwdProp["type"] = "string";
                    cwdProp["description"] = QStringLiteral("Your $PWD (required).");
                    QJsonObject skillProp;
                    skillProp["type"] = "string";
                    skillProp["description"] = QStringLiteral(
                        "Skill identifier, ^[A-Za-z0-9_-]{1,32}$. "
                        "E.g. \"tdd\", \"systematic-debugging\".");
                    QJsonObject stepProp;
                    stepProp["type"] = "integer";
                    stepProp["description"] =
                        QStringLiteral("Current step number (required for set).");
                    QJsonObject phaseProp;
                    phaseProp["type"] = "string";
                    phaseProp["description"] =
                        QStringLiteral("Current phase label (required for set).");
                    QJsonObject notesProp;
                    notesProp["type"] = "array";
                    QJsonObject notesItems;
                    notesItems["type"] = "string";
                    notesProp["items"] = notesItems;
                    notesProp["description"] =
                        QStringLiteral("Optional carry-forward notes.");
                    QJsonObject props;
                    props["op"]         = opProp;
                    props["caller_cwd"] = cwdProp;
                    props["skill"]      = skillProp;
                    props["step"]       = stepProp;
                    props["phase"]      = phaseProp;
                    props["notes"]      = notesProp;
                    schema["properties"] = props;
                    QJsonArray reqArr;
                    reqArr.append(QStringLiteral("op"));
                    reqArr.append(QStringLiteral("caller_cwd"));
                    reqArr.append(QStringLiteral("skill"));
                    schema["required"] = reqArr;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1309 — spec_query: parse a single spec file.
                // Returns {title, status, kind, invariants[]} without
                // reading the full 200-2000 line markdown body.
                {
                    QJsonObject t;
                    t["name"] = "spec_query";
                    t["description"] = QStringLiteral(
                        "Parse a single spec / design markdown file and "
                        "return its parsed metadata + invariant list. "
                        "Returns {ok, id, title, status, kind, path, "
                        "size_bytes, mtime_ms, invariants:[{id, body, "
                        "test_surface?}], invariants_count, source}. "
                        "Recognises both the table form (`| INV-N | "
                        "body | test surface |`) and the bullet form "
                        "(`- **INV-N** — body`). Use when you need "
                        "the contract list of a spec without reading "
                        "the full ~2 K-line body. ID routing: "
                        "<PREFIX>-NNNN → docs/specs/<id>.md (source=specs; "
                        "any project prefix, e.g. ANTS-1963 or DOOM-0009 "
                        "— ANTS-3356; a topic-suffixed file `<id>-*.md` "
                        "such as DOOM-0009-path-tracer.md also resolves); "
                        "phase_<NN>_<topic> → docs/phases/<id>.md "
                        "(source=phases, ANTS-1880). "
                        "GATE DRIFT (ANTS-4352): call with no `id`/`path` and "
                        "mode:\"gate_drift\" to answer \"which gated specs "
                        "have been EDITED since their last review loop?\" — "
                        "returns {stale, current, ungated} with "
                        "commits_since:[{sha, date, subject, same_day?}] on "
                        "each stale row. A Reviewed stamp does not survive an "
                        "edit made while closing a different item, and that "
                        "failure is self-concealing: the document asserts it "
                        "was reviewed, the next session trusts the assertion, "
                        "and the invalidating commit sits where nobody looks. "
                        "`commits_since` is what makes it actionable — the "
                        "subject distinguishes the gate's own fix pass from an "
                        "authoring edit. A spec whose only commits since the "
                        "loop land on the loop DATE is reported current with "
                        "same_day_commits_only:true, because that is the "
                        "gate's own pass and it does not re-arm the gate. "
                        "`ungated` (no loop log) is its own bucket: \"never "
                        "gated\" is a different answer from \"gated and "
                        "current\". "
                        "LIST MODE (ANTS-3360): "
                        "call with NEITHER `id` nor `path` to enumerate the "
                        "specs dir — returns {ok, mode:\"list\", specs_dir, "
                        "specs:[{id, title, status, path, size_bytes, "
                        "mtime_ms}], count, truncated}, the spec-side "
                        "analogue of roadmap_query mode:section_index "
                        "(use it to discover spec ids, not a shell `ls`). "
                        "ANTS-1906 — pass "
                        "an explicit project-relative `path` instead "
                        "of `id` for projects whose specs live "
                        "elsewhere (e.g. "
                        "`docs/phases/phase_22_threading_design.md`); "
                        "the response `source` becomes \"path\" and "
                        "`id` is auto-derived from the basename. Path "
                        "must be project-relative (no leading '/', no "
                        "'..' traversal). Refusals: `bad_id` (an `id` was "
                        "passed that doesn't match the recognised shape), "
                        "`bad_path` (path escaped the project root), "
                        "`not_found` (file absent), `no_project` "
                        "(caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of `Read` when you only need a "
                        "spec's INV list (the contract surface), not "
                        "the full narrative. Typically 5-20× smaller "
                        "than a full Read. Pass `path` for non-Ants "
                        "spec layouts.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject idProp;
                    idProp["type"] = "string";
                    idProp["description"] = QStringLiteral(
                        "Spec ID. <PREFIX>-NNNN → docs/specs/<id>.md "
                        "(any project prefix, e.g. ANTS-1963 or "
                        "DOOM-0009 — ANTS-3356; topic-suffixed "
                        "`<id>-*.md` files also resolve); "
                        "phase_<NN>_<topic> → docs/phases/<id>.md "
                        "(ANTS-1880). Optional when `path` is set "
                        "(ANTS-1906); the explicit `id` then wins as "
                        "the response's display id. Omit BOTH `id` and "
                        "`path` for list mode (ANTS-3360 — enumerate "
                        "the specs dir).");
                    props["id"]         = idProp;
                    QJsonObject pathProp;
                    pathProp["type"] = "string";
                    pathProp["description"] = QStringLiteral(
                        "Optional project-relative path to a spec file "
                        "(e.g. `docs/phases/phase_22_threading_"
                        "design.md`). When set, bypasses id-shape "
                        "routing and reads that file directly; the "
                        "response `id` is auto-derived from the "
                        "basename. Mutually exclusive shape with `id` "
                        "(both are accepted; explicit `id` overrides "
                        "the basename-derived one). Must not start "
                        "with '/' or contain '..' (ANTS-1906).");
                    props["path"]       = pathProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    // ANTS-1906 — only caller_cwd stays unconditionally
                    // required; the verb enforces "id OR path" at
                    // runtime so a single JSON Schema entry can't
                    // express it cleanly without oneOf.
                    req.append("caller_cwd");
                    schema["required"]            = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1963 — spec_log: write the three recurring spec
                // mutations (flip Status / append cold-eyes loop / append
                // INV) instead of hand-editing markdown. Write sibling of
                // spec_query; same id/path routing.
                {
                    QJsonObject t;
                    t["name"] = "spec_log";
                    t["description"] = QStringLiteral(
                        "Edit a spec's structured surface without "
                        "hand-editing markdown. op:\"set_status\" rewrites "
                        "the whole **Status:** field, including any "
                        "continuation lines it wraps onto (ANTS-3785); "
                        "op:\"append_loop\" appends a "
                        "cold-eyes loop-log row, matching the SHAPE the "
                        "section already holds (ANTS-4364): a TABLE row when "
                        "it holds a table — pass `cells`, one string per "
                        "column in the header's order; a bullet from "
                        "`loop_label` + `body` when it holds bullets or does "
                        "not exist yet. Against a table with no `cells` it "
                        "REFUSES format_mismatch, naming the columns it read, "
                        "rather than writing a bullet that corrupts the table. "
                        "The insertion END is INFERRED from the existing rows "
                        "(ANTS-4353), because loop logs run in opposite "
                        "directions across specs in one corpus and a row at "
                        "the wrong end reads as a different loop's result; the "
                        "envelope echoes `row_shape` and `row_order` "
                        "(oldest_first | newest_first | ambiguous, the last "
                        "when fewer than two rows carry a loop number). op:\"append_inv\" appends an INV-N "
                        "bullet at the end of the Invariants section "
                        "(never renumbers). Target via `id` (<PREFIX>-NNNN → "
                        "docs/specs/<id>.md, any project prefix e.g. "
                        "DOOM-0009 — ANTS-3356, topic-suffixed `<id>-*.md` "
                        "also resolves; phase_<NN>_<topic> → "
                        "docs/phases/) or a project-relative `path`. "
                        "Atomic write. Returns {ok, op, id?, path, line, "
                        "bytes_written, file_bytes} plus `previous_status` "
                        "on set_status (ANTS-4114 — the replaced value, so a "
                        "vocabulary mismatch shows here, not at the project's "
                        "lint gate) (ANTS-3724: "
                        "bytes_written is the ADDED-bytes delta, matching "
                        "roadmap_log/changelog_log; file_bytes is the whole "
                        "file); dry_run:true previews line/`bytes` "
                        "without writing. Refusals: `bad_mode`, `bad_id`, "
                        "`bad_path`, `bad_args`, `no_project`, "
                        "`not_found` (file absent), `unrecognised_format` "
                        "(spec present but missing the section/line), "
                        "`write_failed`. caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to flip a spec's Status, log a cold-eyes "
                        "loop, or add an INV — cheaper than an Edit "
                        "round-trip on the markdown.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject opProp; opProp["type"] = "string";
                        { QJsonArray e; e.append(QStringLiteral("set_status"));
                          e.append(QStringLiteral("append_loop"));
                          e.append(QStringLiteral("append_inv"));
                          opProp["enum"] = e; }
                        opProp["description"] = QStringLiteral(
                            "Required. set_status | append_loop | "
                            "append_inv.");
                    QJsonObject idProp; idProp["type"] = "string";
                        idProp["description"] = QStringLiteral(
                            "Spec id (<PREFIX>-NNNN, any project prefix "
                            "e.g. ANTS-1963 or DOOM-0009 — ANTS-3356, or "
                            "phase_<NN>_<topic>). Required unless `path` "
                            "is set.");
                    QJsonObject pathProp; pathProp["type"] = "string";
                        pathProp["description"] = QStringLiteral(
                            "Optional project-relative path to the spec "
                            "file (no leading '/', no '..'). Overrides "
                            "`id` routing.");
                    QJsonObject statusProp; statusProp["type"] = "string";
                        statusProp["description"] = QStringLiteral(
                            "set_status: the replacement value, written "
                            "VERBATIM as one line after \"**Status:** \". "
                            "Any continuation lines the previous value "
                            "Any continuation lines the previous value "
                            "wrapped onto are replaced too — pass "
                            "`preserve_body:true` to KEEP them (ANTS-4136). "
                            "ANTS-4114 — this verb imposes NO vocabulary and "
                            "cannot validate one: each project's permitted "
                            "values live in its own spec standard (Ants uses "
                            "a dated lifecycle, others a fixed word list that "
                            "forbids history), so a value this verb accepts "
                            "may still fail the project's lint gate. Match "
                            "the project's standard; the envelope's "
                            "`previous_status` (on dry_run too) names the "
                            "value being replaced, which is the cheapest "
                            "read of the vocabulary actually in use.");
                    QJsonObject labelProp; labelProp["type"] = "string";
                        labelProp["description"] = QStringLiteral(
                            "append_loop: the loop label "
                            "(e.g. \"Loop 3 (2026-06-03)\").");
                    QJsonObject bodyProp; bodyProp["type"] = "string";
                        bodyProp["description"] = QStringLiteral(
                            "append_loop / append_inv: the bullet body "
                            "prose.");
                    // ANTS-4364 — the table form.
                    QJsonObject cellsProp; cellsProp["type"] = "array";
                    {
                        QJsonObject it; it["type"] = "string";
                        cellsProp["items"] = it;
                    }
                    cellsProp["description"] = QStringLiteral(
                        "append_loop, TABLE form: one string per column, in "
                        "the table header's own order. Required when the loop "
                        "log is a table (the shipped spec skeleton's is); a "
                        "wrong count refuses column_mismatch rather than "
                        "writing a ragged row. Pipes are escaped and newlines "
                        "folded to <br>, so a cell cannot break the column "
                        "count. Ignored for a bullet-shaped log.");
                    QJsonObject invProp; invProp["type"] = "string";
                        invProp["description"] = QStringLiteral(
                            "append_inv: the new INV id (^INV-[0-9]+$). "
                            "Refuses bad_args if already present.");
                    QJsonObject testProp; testProp["type"] = "string";
                        testProp["description"] = QStringLiteral(
                            "append_inv: optional *Test:* clause.");
                    QJsonObject dryRunProp; dryRunProp["type"] = "boolean";
                        dryRunProp["description"] = QStringLiteral(
                            "Optional (ANTS-2136). When true, return the "
                            "resolved landing `line` and `bytes` (would-be "
                            "file size) WITHOUT writing the spec — a free "
                            "pre-flight for the section-routed insert "
                            "(envelope carries dry_run:true).");
                    props["op"]         = opProp;
                    props["id"]         = idProp;
                    props["path"]       = pathProp;
                    props["status"]     = statusProp;
                    {   // ANTS-4136 — keep the Status field's wrapped prose.
                        QJsonObject pb; pb["type"] = "boolean";
                                        pb["default"] = false;
                                        pb["description"] = QStringLiteral(
                            "Optional (ANTS-4136), op:\"set_status\" only. "
                            "Keep the Status field's CONTINUATION LINES and "
                            "rewrite only the opener, instead of replacing the "
                            "whole extent. A spec's Status is a wrapped field "
                            "and this corpus routinely carries a paragraph "
                            "there — review-loop counts, whether the gate "
                            "converged, which sections were most revised — so "
                            "a call whose stated job is to change one word was "
                            "deleting a 490-byte review history. Recoverable "
                            "via `previous_status`, but only if the caller "
                            "notices; this makes the recovery the default. "
                            "It is a LINE rule, not a prose split: there is no "
                            "delimiter this corpus agrees on between the state "
                            "word and the prose after it, so a heuristic would "
                            "drop text on the shapes it guessed wrong — the "
                            "very failure being fixed. Prose on the SAME line "
                            "as the old value is still replaced. Default false, "
                            "so existing callers are unchanged.");
                        props["preserve_body"] = pb;
                    }
                    props["loop_label"] = labelProp;
                    props["cells"]      = cellsProp;  // ANTS-4364
                    props["body"]       = bodyProp;
                    props["inv_id"]     = invProp;
                    props["test"]       = testProp;
                    props["dry_run"]    = dryRunProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("op"));
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1308 — invariant_check: which specs reference
                // a given set of source files, and what invariants do
                // they declare. Surfaces "what contracts could this
                // edit break?" before the edit, not after.
                {
                    QJsonObject t;
                    t["name"] = "invariant_check";
                    t["description"] = QStringLiteral(
                        "Given a list of project-relative file paths "
                        "(typically what you're about to edit), scan "
                        "`docs/specs/*.md` for specs that mention any "
                        "of those paths in their body and return the "
                        "parsed invariant list per matching spec. "
                        "Returns {ok, matched_specs:[{id, path, title, "
                        "matched_terms[], invariants_count}], "
                        "specs_scanned, matched_count, mode, "
                        "invariants_included}. ANTS-3699 — mode defaults "
                        "to \"summary\", which OMITS each spec's "
                        "`invariants:[{id, body}]` bodies (omitted, never "
                        "truncated, so a short list can't read as a "
                        "complete one); `invariants_count` is still the "
                        "true count. That answers this verb's question "
                        "on its own — drill into one spec with "
                        "spec_query. mode:\"full\" restores the bodies, "
                        "which on a widely-referenced file like "
                        "remotecontrol.cpp can exceed the response cap "
                        "alone. Matching is substring-only "
                        "(no symbol resolution) — pass relative paths "
                        "like `src/foo.cpp`, not bare basenames. Use "
                        "BEFORE editing files in `src/` to surface "
                        "documented contracts the edit might break.");
                    t["selection_hint"] = QStringLiteral(
                        "Use with `files:[...]` from `git diff "
                        "--name-only` (or your own changed-file list) "
                        "before editing source files with associated "
                        "specs. Pairs with spec_query for drilling "
                        "into a single matched spec.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject filesProp;
                    filesProp["type"] = "array";
                    QJsonObject itemProp;
                    itemProp["type"] = "string";
                    filesProp["items"] = itemProp;
                    filesProp["minItems"] = 1;
                    filesProp["description"] = QStringLiteral(
                        "Project-relative file paths to scan specs "
                        "for. Substring-matched against spec bodies; "
                        "pass `src/foo.cpp` not `foo.cpp` to avoid "
                        "false hits on basename collisions.");
                    props["files"]      = filesProp;
                    // ANTS-3699 — response-shape selector; summary by default.
                    QJsonObject modeProp;
                    modeProp["type"] = "string";
                    QJsonArray modeEnum;
                    modeEnum.append("summary");
                    modeEnum.append("full");
                    modeProp["enum"]    = modeEnum;
                    modeProp["default"] = "summary";
                    modeProp["description"] = QStringLiteral(
                        "\"summary\" (default) returns each matched spec's "
                        "id/path/title/matched_terms/invariants_count WITHOUT "
                        "the invariant bodies — a few hundred bytes, and the "
                        "whole answer to \"is any of this already under "
                        "contract?\". \"full\" adds `invariants:[{id, body}]`, "
                        "which over a widely-referenced file can exceed the "
                        "response cap. An unknown value refuses `bad_mode`.");
                    props["mode"]       = modeProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("files");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1306 — task_priors: bundle task-start context
                // (matching specs + ROADMAP cards + recent commits +
                // ADRs) for a free-text task description in one call.
                {
                    QJsonObject t;
                    t["name"] = "task_priors";
                    t["description"] = QStringLiteral(
                        "Given a free-text task description, return the "
                        "project context you'd otherwise gather in 6-8 "
                        "exploration round-trips: matching `docs/specs/*.md` "
                        "(with one-line excerpts), matching ROADMAP cards, "
                        "recent commits touching the file paths named in the "
                        "description, and related ADRs. Returns {ok, terms[], "
                        "ids[], paths[], specs:[{id, path, title, excerpt, "
                        "score}], specs_count, roadmap_cards:[{id, status, "
                        "headline, score}], cards_count, commits:[{sha, "
                        "subject, date}], commits_count, adrs:[{path, title, "
                        "score}], adrs_count}. Ranking is case-insensitive "
                        "distinct-needle substring matching; each `*_count` "
                        "is the pre-cap total. Pure composer over "
                        "roadmap_query + git_state + the spec parser — no "
                        "cache. Refusals: `bad_args` (empty description or no "
                        "searchable terms after stopword removal), "
                        "`no_project` (caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use ONCE at task start, before proposing an "
                        "approach, with the user's task description. Cheaper "
                        "and more complete than ad-hoc grep + Read of specs / "
                        "ROADMAP / git log. Drill into a surfaced spec with "
                        "spec_query.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject descProp;
                    descProp["type"] = "string";
                    descProp["description"] = QStringLiteral(
                        "The free-text task description (the user's prompt "
                        "or a sub-task). Parsed into ANTS-NNNN ids, "
                        "file-path tokens, and >=4-char terms for ranking.");
                    props["description"] = descProp;
                    props["caller_cwd"]  = makeCallerCwdReadProp();
                    const struct { const char *name; const char *desc; } caps[] = {
                        {"max_specs",   "Max spec hits to return (default 5, clamp 1-20)."},
                        {"max_cards",   "Max ROADMAP cards to return (default 5, clamp 1-20)."},
                        {"max_commits", "Max commits to return (default 5, clamp 1-20)."},
                        {"max_adrs",    "Max ADRs to return (default 3, clamp 1-20)."},
                    };
                    for (const auto &cap : caps) {
                        QJsonObject p;
                        p["type"] = "integer";
                        p["minimum"] = 1;
                        p["maximum"] = 20;
                        p["description"] = QString::fromUtf8(cap.desc);
                        props[QString::fromUtf8(cap.name)] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("description");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1307 — project_conventions: the task_type-scoped
                // subset of project conventions, each {rule, source}.
                {
                    QJsonObject t;
                    t["name"] = "project_conventions";
                    t["description"] = QStringLiteral(
                        "Return the subset of project conventions relevant "
                        "to a stated task type, instead of re-reading the "
                        "whole CLAUDE.md + standards bundle. Returns {ok, "
                        "task_type, conventions:[{rule, source}], "
                        "conventions_count, sources:[{path, exists}], "
                        "sources_count}. Each `rule` is a one-line "
                        "convention; `source` is the repo-relative doc that "
                        "states it in full (read it for detail). `sources[]` "
                        "is the deduped source set with an existence flag "
                        "(a `false` flags a missing/renamed doc). Static "
                        "curated table; no file-body reads. Refusals: "
                        "`bad_args` (task_type not one of feature/bugfix/"
                        "refactor/docs/test), `no_project` (caller_cwd "
                        "unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use at task start with the kind of work you're "
                        "about to do (feature/bugfix/refactor/docs/test) to "
                        "get just the relevant rules + their authoritative "
                        "doc paths — cheaper than reading the full CLAUDE.md "
                        "preamble.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject typeProp;
                    typeProp["type"] = "string";
                    QJsonArray typeEnum;
                    typeEnum.append("feature");
                    typeEnum.append("bugfix");
                    typeEnum.append("refactor");
                    typeEnum.append("docs");
                    typeEnum.append("test");
                    typeProp["enum"] = typeEnum;
                    typeProp["description"] = QStringLiteral(
                        "The kind of work about to be done. Lower-case; one "
                        "of feature, bugfix, refactor, docs, test.");
                    props["task_type"]  = typeProp;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("task_type");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1299 — build_status: record/read the most recent
                // build's outcome at <root>/.audit_cache/build.json.
                {
                    QJsonObject t;
                    t["name"] = "build_status";
                    t["description"] = QStringLiteral(
                        "Record or read the most recent build's outcome. "
                        "op=record takes {exit_code, output, started_at_ms?, "
                        "finished_at_ms?} and parses GCC/clang/cppcheck-2.x "
                        "compiler output into errors[] + warnings_count; "
                        "writes <root>/.audit_cache/build.json atomically. "
                        "op=read (default) returns the cached envelope OR "
                        "{ok:false, code:\"not_cached\"} when no record "
                        "exists. Read also surfaces stale:true when any "
                        "compile-input file (src/, tests/, cmake/, "
                        "CMakeLists.txt) has mtime newer than the cache. "
                        "Saves ~3-10 K tokens per build cycle vs. shelling "
                        "`cmake --build` and reading the full output.");
                    t["selection_hint"] = QStringLiteral(
                        "Use op=record right after every `cmake --build` "
                        "you run, then op=read on subsequent steps "
                        "instead of re-shelling the build. Pairs with "
                        "test_results for the build → test → commit gate.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject opProp;
                        opProp["type"]        = "string";
                        QJsonArray opEnum;
                        opEnum.append("read");
                        opEnum.append("record");
                        opProp["enum"]        = opEnum;
                        opProp["default"]     = "read";
                        opProp["description"] = QStringLiteral(
                            "Verb. \"read\" (default) returns the "
                            "cached envelope; \"record\" parses the "
                            "supplied output and writes the cache.");
                        props["op"] = opProp;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Build exit code (required for op=record).");
                        props["exit_code"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Full stdout+stderr of the build "
                            "(required for op=record).");
                        props["output"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the build started "
                            "(optional, op=record).");
                        props["started_at_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the build finished "
                            "(optional, op=record).");
                        props["finished_at_ms"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1300 — test_results: record/read the most recent
                // ctest --output-on-failure run.
                {
                    QJsonObject t;
                    t["name"] = "test_results";
                    t["description"] = QStringLiteral(
                        "Record or read the most recent ctest run's "
                        "summary. op=record takes {exit_code, output, "
                        "started_at_ms?, finished_at_ms?, duration_ms?} "
                        "and parses ctest --output-on-failure into "
                        "{passed, failed, skipped, total, failing_tests:"
                        "[{name, excerpt}]}; writes "
                        "<root>/.audit_cache/tests.json atomically. "
                        "op=read (default) returns the cached envelope. "
                        "op=read with detail=<name> returns one failing "
                        "test's full captured block (up to 8 000 chars) "
                        "instead of the truncated 20-line excerpt. "
                        "Refusals: not_cached (no record), "
                        "detail_not_found (cache exists, named test "
                        "wasn't in failing_tests[]). Saves ~3-15 K "
                        "tokens per test cycle.");
                    t["selection_hint"] = QStringLiteral(
                        "Use op=record after every `ctest "
                        "--output-on-failure`, then op=read for "
                        "subsequent checks. Pass detail=<name> when you "
                        "need the full body of one failing test "
                        "without re-running the suite.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject opProp;
                        opProp["type"]        = "string";
                        QJsonArray opEnum;
                        opEnum.append("read");
                        opEnum.append("record");
                        opProp["enum"]        = opEnum;
                        opProp["default"]     = "read";
                        opProp["description"] = QStringLiteral(
                            "Verb. \"read\" (default) returns the "
                            "cached envelope; \"record\" parses the "
                            "supplied output and writes the cache.");
                        props["op"] = opProp;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Ctest exit code (required for op=record).");
                        props["exit_code"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Full stdout+stderr of the ctest run "
                            "(required for op=record).");
                        props["output"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the ctest run "
                            "started (optional, op=record).");
                        props["started_at_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Wall-clock ms when the ctest run "
                            "finished (optional, op=record).");
                        props["finished_at_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Total duration in ms (optional, "
                            "op=record). Caller-supplied wins over "
                            "the parsed `Total Test time (real)` "
                            "footer.");
                        props["duration_ms"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Failing-test name selector (optional, "
                            "op=read only). When present, response "
                            "shape is {ok, detail:{name, excerpt}} "
                            "with the full captured block as "
                            "excerpt. detail_not_found refusal "
                            "when the name isn't in failing_tests[].");
                        props["detail"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["etag_match"] = makeEtagMatchProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1302 — focused_test: run only the ctest subset
                // touching the changed files, return the test_results
                // envelope shape.
                {
                    QJsonObject t;
                    t["name"] = "focused_test";
                    t["description"] = QStringLiteral(
                        "Run only the tests that exercise the changed "
                        "files, instead of the whole suite. Resolves "
                        "`changed_files` (or auto-derives from git "
                        "status) to ctest -R patterns via "
                        "`tests/coverage-map.json` (or a heuristic, or a "
                        "full-suite fallback), runs `ctest --test-dir "
                        "<build> -R <regex> --output-on-failure`, and "
                        "returns the test_results envelope {ok, passed, "
                        "failed, skipped, total, failing_tests[]} plus "
                        "{selection (map/heuristic/full), ctest_filter, "
                        "changed_files, mapped_files, unmapped_files, "
                        "ignored_files, selection_reason, build_dir, "
                        "duration_ms}. Conservative: an unmapped source "
                        "file or a 0-match selection falls back to the "
                        "full suite (sets downgraded_to_full) — it never "
                        "reports green from a run that matched no tests. "
                        "Runs against the EXISTING build (does not "
                        "rebuild). Refusals: no_project, no_build_dir "
                        "(no configured build/ with CMakeCache.txt), "
                        "focused_test_in_flight, bad_args, ctest_missing, "
                        "ctest_failed (timeout/crash), unrecognised_output.");
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of a full `ctest` to verify a focused "
                        "change (5x-50x faster on mapped files). Build "
                        "FIRST — focused_test does not rebuild. Omit "
                        "`changed_files` to use git status, or pass a set.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject item; item["type"] = "string";
                        p["items"] = item;
                        p["description"] = QStringLiteral(
                            "Project-relative changed file paths. When "
                            "omitted, derived from git working-tree "
                            "status. Mapped to ctest patterns via "
                            "tests/coverage-map.json.");
                        props["changed_files"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]    = "integer";
                        p["minimum"] = 10;
                        p["maximum"] = 1800;
                        p["description"] = QStringLiteral(
                            "ctest wall-clock budget in seconds "
                            "(default 300, clamp 10-1800).");
                        props["timeout_sec"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Build dir relative to the project root "
                            "(default \"build\"). Must contain "
                            "CMakeCache.txt.");
                        props["build_dir"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-3745 — build_target_for: which target owns a file,
                // and the two commands that answer is for.
                {
                    QJsonObject t;
                    t["name"] = "build_target_for";
                    t["description"] = QStringLiteral(
                        "[build] Which build target owns this source file, "
                        "read statically from CMakeLists.txt — plus the "
                        "`cmake --build --target` line and, for a gtest "
                        "source, the `ctest -R` filter its suites imply. "
                        "Replaces the two fallbacks this project kept "
                        "reaching for: an awk walking backwards to the "
                        "nearest ants_add_*_bundle, and running every "
                        "build/test_* with --gtest_list_tests to find which "
                        "binary carried a suite. The mapping is NOT "
                        "guessable from the path — tests/features/"
                        "cold_eyes_engine builds into test_audit, and "
                        "tests/features/spec_conformance into test_claude — "
                        "so recall does not substitute. Returns {ok, path, "
                        "cmake_path, targets:[{name, kind, command, line, "
                        "source_count, build_command}], targets_parsed, "
                        "found, suites?, ctest_filter?, ctest_command?, "
                        "hint?}. `found:false` is a real answer rather than "
                        "a refusal: a header is usually not listed (build "
                        "the target owning its .cpp), and a NEW test source "
                        "is not listed until it is added to a bundle's "
                        "SOURCES — the trap where the build then succeeds "
                        "silently and runs the old binary, so check `found` "
                        "after adding one. A source named through a CMake "
                        "variable, a generator expression or "
                        "target_sources() is not resolved and reports "
                        "unowned rather than being attributed to the wrong "
                        "target. Multiple owners are possible and all are "
                        "returned. Read-only; opens the CMake file and (for "
                        "suites) the source. caller_cwd required.");
                    t["selection_hint"] = QStringLiteral(
                        "Use right after editing or adding a test source and "
                        "before building — it answers what to build and what "
                        "to run in one call.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Project-relative (or absolute in-root) source "
                            "path to look up. Required.");
                        props["path"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "CMake file to parse, project-relative "
                            "(default \"CMakeLists.txt\").");
                        props["cmake_path"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Build dir used when composing build_command / "
                            "ctest_command (default \"build\"). Nothing is "
                            "run and the directory is not probed.");
                        props["build_dir"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]    = "boolean";
                        p["default"] = true;
                        p["description"] = QStringLiteral(
                            "Read the source for TEST/TEST_F/TEST_P suite "
                            "names and emit suites[] + ctest_filter. Pass "
                            "false to skip the second file read when only "
                            "the target name is wanted.");
                        props["suites"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req2;
                    req2.append("caller_cwd");
                    req2.append("path");
                    schema["required"]             = req2;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1303 — find_definition: tree-wide regex scan for
                // a symbol's definition/declaration line(s).
                {
                    QJsonObject t;
                    t["name"] = "find_definition";
                    t["description"] = QStringLiteral(
                        "Find where a symbol is defined across the "
                        "project, without a full LSP. Regex-anchored "
                        "scan over C++/Python/Lua/Shell source. Returns "
                        "{ok, symbol, lang, definitions:[{file, line, "
                        "signature, lang, kind}], definitions_count, "
                        "files_scanned, truncated, walk_capped}. `kind` "
                        "is \"definition\" or \"declaration\" (C++ "
                        "header decls end in `;`). When there are zero "
                        "definitions but the query exactly matches a "
                        "source file's base name, adds `file_stem_hint` "
                        "(the rel path) + a ready-to-read `hint` string "
                        "(\"no symbol named X; did you mean the file "
                        "X.cpp?\") — ANTS-1950, saves a follow-up when the "
                        "query was a filename, not a symbol. Use instead "
                        "of 4-6 grep + Read cycles for \"where is Foo "
                        "defined?\". Refusals: bad_args (symbol missing "
                        "or not a valid identifier), no_project "
                        "(caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of grep+Read when you need the "
                        "definition site of one named symbol. Pairs "
                        "with find_caller (who uses it) and file_outline "
                        "(what's in one file).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Identifier to locate "
                            "(^[A-Za-z_][A-Za-z0-9_]{0,127}$).");
                        props["symbol"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append("auto"); e.append("cpp"); e.append("py");
                        e.append("lua");  e.append("sh");
                        e.append("generic"); e.append("glsl");  // ANTS-2150 / ANTS-3558
                        p["enum"]        = e;
                        p["default"]     = "auto";
                        p["description"] = QStringLiteral(
                            "Restrict the scan to one language family. "
                            "\"auto\" (default) scans every supported family "
                            "(cpp, py, lua, sh, generic, glsl).");
                        props["lang"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on definitions[] (default 50). "
                            "definitions_count carries the pre-cap "
                            "total.");
                        props["max_results"] = p;
                    }
                    {
                        // ANTS-2087 — opt-in symbol body inline.
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, each definition carries its body "
                            "inline (`body`, `body_start_line`, "
                            "`body_end_line`, `body_truncated?`) via "
                            "read_region's symbol-body extractor — answers "
                            "\"where is Foo AND what does it do\" in one "
                            "call instead of a follow-up read_region. Off "
                            "by default (lean envelope). A def whose body "
                            "the file outline can't resolve "
                            "(declaration-only, overload ambiguity) is "
                            "returned without a body (ANTS-2087).");
                        props["include_body"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("symbol");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1303 — find_caller: tree-wide regex scan for the
                // call sites of a symbol, plus its best-guess definition.
                {
                    QJsonObject t;
                    t["name"] = "find_caller";
                    t["description"] = QStringLiteral(
                        "Find what calls a symbol across the project, "
                        "without a full LSP. Regex-anchored scan over "
                        "C++/Python/Lua/Shell source. Returns {ok, "
                        "symbol, lang, callers:[{file, line, context, "
                        "lang}], callers_count, definition?, "
                        "files_scanned, truncated, walk_capped}. The "
                        "symbol's own definition line is excluded from "
                        "callers; `definition` carries the best-guess "
                        "definition so you get \"where + who\" in one "
                        "call. With `files_only:true` the `callers[]` "
                        "array is replaced by `files:[{file, count, "
                        "lines[]}]` (the matched-file manifest, no "
                        "quoted context windows). `lane` scopes the scan "
                        "to one subdirectory (ANTS-3805) — reach for it on "
                        "a short generic name like update/render/init, "
                        "which otherwise matches every class that has one. "
                        "Refusals: bad_args "
                        "(symbol missing or not a valid identifier), "
                        "no_project (caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use instead of grep+Read for \"who calls Foo?\". "
                        "Pairs with find_definition; shell matches are "
                        "coarser (no call-paren convention).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Identifier to locate "
                            "(^[A-Za-z_][A-Za-z0-9_]{0,127}$).");
                        props["symbol"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append("auto"); e.append("cpp"); e.append("py");
                        e.append("lua");  e.append("sh");
                        e.append("generic"); e.append("glsl");  // ANTS-2150 / ANTS-3558
                        p["enum"]        = e;
                        p["default"]     = "auto";
                        p["description"] = QStringLiteral(
                            "Restrict the scan to one language family. "
                            "\"auto\" (default) scans every supported family "
                            "(cpp, py, lua, sh, generic, glsl).");
                        props["lang"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on callers[] (default 200). "
                            "callers_count carries the pre-cap total.");
                        props["max_results"] = p;
                    }
                    {
                        // ANTS-3805 — scope filter. Without one, a generic
                        // method name (update/render/init/reset — most of a
                        // real class API) returns hundreds of unnarrowable
                        // rows and the verb silently fails at its headline
                        // question. Reported by Vestige: 360 truncated callers
                        // for `update`, none in the class they changed.
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Optional (ANTS-3805). Restrict the scan to this "
                            "project-relative subdirectory (e.g. \"src/render\"). "
                            "Scopes the WALK, so callers_count and truncated "
                            "describe the scoped set rather than a filtered "
                            "view of a wider one; the applied value is echoed "
                            "back as `lane`. Use it whenever the symbol is a "
                            "short generic method name — a bare `update` "
                            "matches every class that has one. A lane that "
                            "does not resolve to a directory under the root is "
                            "IGNORED (you get the whole-project scan), never a "
                            "silent empty result.");
                        props["lane"] = p;
                    }
                    {
                        // ANTS-2087 — opt-in body of the called symbol's
                        // definition (not the call sites).
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, the `definition` (the called "
                            "symbol's own definition) carries its body "
                            "inline (`body`, `body_start_line`, "
                            "`body_end_line`, `body_truncated?`) via "
                            "read_region's symbol-body extractor. The "
                            "call sites in `callers[]` already carry "
                            "context lines and are unaffected. Off by "
                            "default (ANTS-2087).");
                        props["include_body"] = p;
                    }
                    {
                        // ANTS-3555 — files_only: manifest mode. Drop the
                        // quoted per-call `context` windows; return the
                        // matched-file set + exact line numbers instead.
                        QJsonObject p;
                        p["type"]    = "boolean";
                        p["default"] = false;
                        p["description"] = QStringLiteral(
                            "When true, return {files:[{file, count, "
                            "lines[]}], files_count, callers_count, "
                            "files_only:true, definition?, ...} — the "
                            "DISTINCT set of files that call the symbol, each "
                            "with its call count and the exact line numbers, "
                            "and NO quoted `context` windows. A "
                            "rows-ELIMINATED manifest for \"which files call "
                            "X?\" when you will read_region the sites next. "
                            "`count` is the returned (possibly "
                            "max_results-capped) call count for that file and "
                            "equals lines[].size(); top-level callers_count "
                            "carries the true pre-cap total. Off by default "
                            "(ANTS-3555).");
                        props["files_only"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    props["encoding"] = makeEncodingProp();      // ANTS-2090
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("symbol");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1305 — similar_code: tree-wide shape matcher.
                // Ranks existing class/function signatures by token-set
                // similarity to a free-text shape query.
                {
                    QJsonObject t;
                    t["name"] = "similar_code";
                    t["description"] = QStringLiteral(
                        "Find the project's existing examples of a code "
                        "shape before you write a new one (CLAUDE.md §3 "
                        "reuse-before-rewriting). Walks C++/Python source, "
                        "extracts class/function signatures, and ranks "
                        "them by token-set Jaccard similarity to your "
                        "`shape` query. Returns {ok, shape, lang, "
                        "matches:[{file, line, signature, kind, lang, "
                        "score}], matches_count, files_scanned, "
                        "truncated, walk_capped}, top matches first "
                        "(default 3, max 20). The query is tokenised, "
                        "never run as a regex. Refusals: bad_args (shape "
                        "missing/empty/over-512-chars/no usable tokens), "
                        "no_project (caller_cwd unresolved).");
                    t["selection_hint"] = QStringLiteral(
                        "Use before writing a new dialog / IPC verb / "
                        "test to copy the project convention. Phrase the "
                        "query like the signature you are about to write "
                        "(e.g. \"class FooDialog : public QDialog\"), not "
                        "as English prose. Pairs with file_outline.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"]        = "string";
                        p["description"] = QStringLiteral(
                            "Free-text code shape to match, phrased like "
                            "a signature (e.g. \"void cmdBar(const "
                            "QJsonObject&)\"). Tokenised, not a regex.");
                        props["shape"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        QJsonArray e;
                        e.append("auto"); e.append("cpp"); e.append("py");
                        p["enum"]        = e;
                        p["default"]     = "auto";
                        p["description"] = QStringLiteral(
                            "Restrict the scan to one language family. "
                            "\"auto\" (default) scans C++ and Python.");
                        props["lang"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"]        = "integer";
                        p["description"] = QStringLiteral(
                            "Cap on matches[] (default 3, max 20). "
                            "matches_count carries the pre-cap total.");
                        props["max_results"] = p;
                    }
                    {
                        // ANTS-2156 — return the FULL enclosing definition
                        // per match so the idiom is copyable in one call.
                        QJsonObject p;
                        p["type"]        = "boolean";
                        p["default"]     = false;
                        p["description"] = QStringLiteral(
                            "When true, each match also carries the FULL "
                            "enclosing definition (`symbol`, `body` lines, "
                            "`body_start_line`/`body_end_line`, "
                            "`body_truncated`) extracted via read_region — "
                            "so you copy the canonical in-repo idiom in ONE "
                            "call instead of opening each file. A match whose "
                            "signature doesn't resolve to an outline symbol "
                            "carries `body_unavailable:true` (file:line still "
                            "given). Pair with a small max_results (the "
                            "default 3).");
                        props["include_bodies"] = p;
                    }
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("shape");
                    req.append("caller_cwd");
                    schema["required"]             = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }

                // ANTS-1112 — five `indie_review_*` tools that lift the
                // mechanical halves of /indie-review out of orchestrator
                // context. Engine: src/indiereviewengine.{h,cpp}.
                {
                    QJsonObject t;
                    t["name"] = "indie_review_partition";
                    t["description"] = QStringLiteral(
                        "Return the subsystem (lane) partition for "
                        "indie-review. Reads the module map from "
                        "`docs/subsystems.md` (or CLAUDE.md `## Module map "
                        "(src/)` when un-migrated; or the "
                        "`.indie-review/partition.json` override if "
                        "present) and computes per-lane source-file lists. "
                        "Saves N file-walk passes the orchestrator would "
                        "otherwise do. The envelope also carries "
                        "`suggested_merges:[{lanes:[a,b],rationale}]` "
                        "(ANTS-1288) — lanes whose summaries duplicate each "
                        "other (e.g. a multi-name module-map bullet) so you "
                        "can fold them into one review unit rather than "
                        "dispatching two near-identical briefs. ANTS-4100 — "
                        "each lane carries `file_count` (a real walk; a lane "
                        "may name a directory), and a lane above 30 files is "
                        "marked `too_coarse:true`, mirrored by envelope "
                        "`too_coarse` / `too_coarse_lanes` / "
                        "`too_coarse_hint`. The verb mirrors the module map's "
                        "granularity and never splits a lane for you: treat "
                        "`too_coarse` as a prompt to partition by cohesion "
                        "and commit `.indie-review/partition.json`.");
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
                        "Required: lane (string). **Rate limit "
                        "(ANTS-1643):** BriefAssembly tier — 30 calls "
                        "/ 60 s per (tool, caller_cwd). A canonical "
                        "`/indie-review` Phase-2 fan-out dispatches "
                        "12-16 briefs in one parallel batch, which "
                        "fits comfortably under this cap. Higher "
                        "than the 10/min Expensive tier the sibling "
                        "`indie_review_*` verbs sit in. "
                        "Optional (ANTS-3375 / ANTS-3493): "
                        "source_paths[] — when `lane` is not in the "
                        "auto-partition, the brief is synthesised from "
                        "these caller-supplied changed-file paths "
                        "instead. The code-review analogue of "
                        "cold_eyes_brief's doc_paths[] (ANTS-1508): use "
                        "it as a lightweight single-reviewer broker for a "
                        "Rule-8 cold review of a small code / dependency "
                        "diff, without committing a "
                        ".indie-review/partition.json. Paths must be "
                        "project-relative and resolve inside the project "
                        "root (traversal-guarded); non-resolving / "
                        "escaping entries are refused and surfaced in "
                        "`source_paths_rejected`.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to assemble the brief for one "
                        "indie-review chunk. Run after "
                        "indie_review_partition. Pass source_paths[] "
                        "with an ad-hoc lane label for a cold review of "
                        "a changed-file set / dependency diff.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject laneProp;
                    laneProp["type"] = "string";
                    laneProp["description"] = QStringLiteral(
                        "Lane name as returned by indie_review_partition. "
                        "In source_paths[] ad-hoc mode (ANTS-3375) this "
                        "is any label you choose for the changed-file set.");
                    // ANTS-3375 / ANTS-3493 — optional ad-hoc source set.
                    QJsonObject sourcePathsProp;
                    sourcePathsProp["type"] = "array";
                    QJsonObject sourcePathsItems;
                    sourcePathsItems["type"] = "string";
                    sourcePathsProp["items"] = sourcePathsItems;
                    sourcePathsProp["description"] = QStringLiteral(
                        "Optional (ANTS-3375 / ANTS-3493). When `lane` is "
                        "absent from the auto-partition, synthesise an "
                        "ad-hoc review lane over these project-relative "
                        "source paths (e.g. the changed files of a "
                        "dependency bump / code diff). Mirrors "
                        "cold_eyes_brief's doc_paths[]. Each path is "
                        "traversal-guarded; entries that escape the root "
                        "or don't exist are refused and listed in "
                        "`source_paths_rejected`.");
                    QJsonObject props;
                    props["lane"] = laneProp;
                    props["source_paths"] = sourcePathsProp;
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
                        "min_lanes (default 2); "
                        "allow_outside_project (ANTS-3713) to point "
                        "reports_dir at an absolute path such as the "
                        "session scratchpad. A citation is matched as "
                        "`path/to/file.ext:LINE` (bold wrapping and a "
                        "`423-424` range both parse; the range's first "
                        "number is used); ANTS-4095 — a citation naming only "
                        "a basename resolves when that basename is UNIQUE in "
                        "the tree, and stays dropped when it is ambiguous. "
                        "The envelope reports `citations_seen` / "
                        "`citations_resolved` (+ `citations_by_basename`), "
                        "and sets `unresolved_citations:true` when reports "
                        "parsed but nothing resolved — that case is NOT the "
                        "same as no two lanes agreeing. `total_input_bytes` "
                        "is 0 by design on the reports_dir path and is not a "
                        "parse-failure signal.");
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
                        "*.md files — or, with "
                        "allow_outside_project:true, an absolute one "
                        "(ANTS-3713). Lane name = filename stem. "
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
                    // ANTS-3713 — same opt-in name and posture as
                    // test_audit_synthesis_prompt's (ANTS-1455).
                    QJsonObject aopProp;
                    aopProp["type"] = "boolean";
                    aopProp["description"] = QStringLiteral(
                        "ANTS-3713 — when true, reports_dir may resolve "
                        "outside the project root (session scratchpad, "
                        "/tmp), so lane reports need not be written into "
                        "the working tree. Still NFC + control-char "
                        "checked and canonicalised. Default false.");
                    QJsonObject props;
                    props["reports"]     = reportsProp;
                    props["reports_dir"] = reportsDirProp;
                    props["min_lanes"]   = minLanesProp;
                    props["allow_outside_project"] = aopProp;
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
                        "the block into ROADMAP.md. ANTS-1644 — pass "
                        "`narrative_mode:true` + `narrative_md:\"<pre-"
                        "rendered markdown>\"` to insert prose under "
                        "the section heading verbatim, skipping ID "
                        "allocation and per-finding bullet rendering. "
                        "Use this when the natural shape is one prose "
                        "subsection (Closed inline / Deferred / "
                        "False-positives) rather than N bullets the "
                        "reviewer doesn't want. Required: caller_cwd "
                        "(string — your $PWD; ANTS-1372 cross-project "
                        "gate). One of: actionable (array) OR "
                        "narrative_mode=true + narrative_md.");
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
                        "objects describing the corroborated set. "
                        "ANTS-1278 — each object MAY also carry "
                        "{title, description, layman, kind} to "
                        "render a real roadmap card (bold title + "
                        "body + Layman: + Kind:). Omit any of them "
                        "and the renderer emits a LOUD "
                        "`**TODO: describe this finding (cited by "
                        "N lanes at file:line).**` placeholder so "
                        "a caller cannot ship a stub bullet.");
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
                        "Your $PWD — the project this fold-in writes to. "
                        "Anchored to caller_cwd (ANTS-1630): the write "
                        "lands on YOUR project's ROADMAP regardless of "
                        "which Ants tab is focused; refuses cwd_bad if "
                        "caller_cwd doesn't resolve to a directory.");
                    // ANTS-1644 — narrative-mode escape hatch.
                    QJsonObject nmProp;
                    nmProp["type"] = "boolean";
                    nmProp["description"] = QStringLiteral(
                        "Opt out of per-finding bullet rendering. "
                        "When true, the handler inserts "
                        "`narrative_md` verbatim under the "
                        "`### 🔍 Indie-review fold-in (<DATE>)` "
                        "heading, skips `.roadmap-counter` "
                        "allocation, and returns `allocated_ids:[]`. "
                        "`actionable` is then optional. Use for "
                        "prose subsections (Closed inline / "
                        "Deferred / False-positives) where N bullets "
                        "would be noise.");
                    QJsonObject nmdProp;
                    nmdProp["type"] = "string";
                    nmdProp["description"] = QStringLiteral(
                        "Pre-rendered markdown body for "
                        "`narrative_mode:true`. Inserted verbatim "
                        "after a blank line under the section "
                        "heading. Trailing newline added if absent. "
                        "Refused with code "
                        "`narrative_md_required` when empty or "
                        "whitespace-only.");
                    QJsonObject props;
                    props["actionable"]             = actProp;
                    props["date_iso"]               = dateProp;
                    props["release_block_heading"]  = hdrProp;
                    props["narrative_mode"]         = nmProp;
                    props["narrative_md"]           = nmdProp;
                    props["dry_run"]                = makeDryRunProp();  // ANTS-2227
                    props["id_prefix"]              = makeFoldInIdPrefixProp();  // ANTS-3498
                    props["caller_cwd"]             = callerProp;
                    schema["properties"] = props;
                    // ANTS-1644 — `actionable` dropped from required.
                    // Handler still refuses with bad_args when both
                    // narrative_mode=false AND actionable[] empty.
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1279 — indie_review_orchestrate (single-call
                // dispatch manifest for a Claude-Code-driven sweep).
                {
                    QJsonObject t;
                    t["name"] = "indie_review_orchestrate";
                    t["description"] = QStringLiteral(
                        "One call that returns the whole dispatch plan for "
                        "an /indie-review sweep — replaces "
                        "indie_review_partition + N indie_review_brief "
                        "calls. Returns {ok, lane_count, reports_dir, "
                        "lanes:[{name, summary, source_paths, report_path, "
                        "brief, contract_docs, byte_count}], "
                        "suggested_merges, next_steps}. Each lane's `brief` "
                        "is the v2 manifest (no inlined source bodies — the "
                        "subagent Reads source_paths itself), so the "
                        "response stays compact. Dispatch one Agent per "
                        "lane (folding any suggested_merges pair first), "
                        "have each Write its review to "
                        "`<project_root>/<report_path>`, then call "
                        "indie_review_corroborate (reports_dir=<reports_dir>) "
                        "+ indie_review_fold_in to collect. Pass "
                        "include_briefs:false for a tiny skeleton (names + "
                        "paths only). Required: caller_cwd (ANTS-1404). "
                        "See docs/specs/ANTS-1279.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to kick off a full multi-lane indie-review in "
                        "one call instead of partition + per-lane briefs. "
                        "Collection reuses indie_review_corroborate + "
                        "indie_review_fold_in.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    QJsonObject ibProp;
                    ibProp["type"]    = "boolean";
                    ibProp["default"] = true;
                    ibProp["description"] = QStringLiteral(
                        "When true (default), each lane carries its full "
                        "`brief` manifest + contract_docs. When false, "
                        "return only the skeleton (name / summary / "
                        "source_paths / report_path) for a tiny response.");
                    props["include_briefs"] = ibProp;
                    // ANTS-1391 — caller_cwd anchor.
                    props["caller_cwd"] = makeCallerCwdReadProp();
                    schema["properties"] = props;
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
                        "Use to run the review lanes on the configured "
                        "LOCAL AI endpoint instead of Claude subagents — "
                        "a weaker reviewer, chosen for cost, not parity. "
                        "For a Claude-subagent plan use "
                        "indie_review_orchestrate.");
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
                        "tag or HEAD~10), categories (subset of the four), "
                        "limit/offset (page the findings array — default "
                        "limit 100, max 500; by_category always counts the "
                        "full scan). Envelope carries total_findings, "
                        "returned, has_more, next_offset (ANTS-3345).");
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
                    // ANTS-3345 — pagination over the findings array.
                    QJsonObject limitProp; limitProp["type"] = "integer";
                    limitProp["description"] = QStringLiteral(
                        "Max findings to return in this page. Default 100, "
                        "clamped to [1,500]. by_category still counts the "
                        "full scan.");
                    QJsonObject offsetProp; offsetProp["type"] = "integer";
                    offsetProp["description"] = QStringLiteral(
                        "0-based index into the full finding list. Page with "
                        "next_offset from the prior response. Default 0.");
                    QJsonObject props;
                    props["since"]      = sinceProp;
                    props["categories"] = catProp;
                    props["limit"]      = limitProp;
                    props["offset"]     = offsetProp;
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
                        "no-op (file_changed / not_fixable). Pass "
                        "dry_run:true to run every guard + compute the "
                        "patch but skip the write — the envelope then "
                        "carries {dry_run:true, would_apply} (applied "
                        "stays false). Required: "
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
                    props["dry_run"]      = makeDryRunProp();   // ANTS-2227
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
                        "inserts the block into ROADMAP.md. Refuses "
                        "(needs_triage) a bulk batch of >25 findings unless "
                        "triaged:true — a raw scan is a mix of real, FP-prone, "
                        "and mechanical findings that must be reviewed, not "
                        "dumped (ANTS-3346). Required: deferred (array), "
                        "caller_cwd (string — your $PWD; ANTS-1372).");
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
                    // ANTS-3346 — triage gate override.
                    QJsonObject triagedProp; triagedProp["type"] = "boolean";
                    triagedProp["description"] = QStringLiteral(
                        "Assert this batch was reviewed. Required to defer "
                        ">25 findings at once; without it such a batch is "
                        "refused (needs_triage) to prevent dumping raw scan "
                        "output into ROADMAP. Defaults to false.");
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
                    props["triaged"]               = triagedProp;   // ANTS-3346
                    props["dry_run"]               = makeDryRunProp();   // ANTS-2227
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
                        "Run the project's build / test / lint gates and "
                        "return pass/fail with log tails. Reads "
                        ".ants/verify.json (or auto-detects CMakePresets/"
                        "package.json/Cargo.toml/pyproject.toml). Args: "
                        "gates (subset of build|tests|lint), lines, "
                        "timeout_sec [10,1800], force_refresh, cache_only "
                        "(read-only, bypasses the cwd gate). Results cached "
                        "5 min keyed on git state; responses carry "
                        "cache_hit. Two-tier timeouts (tool-side per-gate "
                        "vs transport ~60s); for builds >60s fall back to "
                        "Bash cmake/make. Required: caller_cwd.");
                    // ANTS-2079 — full timeout / phase-timing reference in
                    // `detail` (stripped from the tools/list wire; served
                    // by tool_info {name:"verify_changes"}).
                    t["detail"] = QStringLiteral(
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
                        "$PWD; ANTS-1372 cross-project gate). ANTS-1497 "
                        "(also ANTS-1621): `cache_only:true` is a "
                        "read-only call that **bypasses** the cross-"
                        "project cwd gate, so a session in project B "
                        "can probe its own cache while Ants happens to "
                        "focus a different project's tab. Full runs "
                        "(non-cache-only) still require the focused-tab "
                        "match; for those, fall back to Bash → "
                        "`cmake/make` when Ants is focused elsewhere. "
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
                        "is independent of the transport cap. "
                        "**Phase timing (ANTS-1628):** every successful "
                        "envelope carries `wall_clock_ms`, `pre_gate_ms`, "
                        "and `gate_ms`. A large `pre_gate_ms` with a "
                        "tiny `gate_ms` means the wrapper work (config "
                        "load, cache lookup, trust probe) is consuming "
                        "the transport budget before the build starts — "
                        "an actionable signal distinct from a long build.");
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
                        "per-tool wall-clock cap (default 30 s, [5, 300]; "
                        "ANTS-3585 raised the ceiling for big C/C++ sweeps — "
                        "a high cap is meant for the async path), "
                        "aggregate cap min(N*cap*1.5, 900 s). Caller "
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
                        "ANTS-2032: the envelope carries `partial` (true "
                        "when a tool timed out / crashed but the rest of "
                        "the run still produced results + the SARIF "
                        "artifact) and `incomplete_tools[]` naming the "
                        "offenders, so a single tool blowing its cap never "
                        "yields an all-or-nothing empty result. "
                        "ANTS-4371: every zero ships with its denominator — "
                        "per tool `paths_given` + `scanned_whole_project` (and "
                        "`no_files:true` when a NARROWED scope matched "
                        "nothing), plus top-level `paths_given_total` / "
                        "`scanned_whole_project` / `tools_with_no_files[]`. "
                        "Read them before treating total_raw:0 as clean: a "
                        "zero-finding audit is the most consequential result "
                        "this verb returns. They are deliberately NOT folded "
                        "into `partial` / `incomplete_tools` — a narrowed "
                        "scope that legitimately matched nothing is not a "
                        "failed run. "
                        "ANTS-3585: `incomplete_tools_detail[]` says WHY each "
                        "is incomplete ({tool, status, elapsed_ms, truncated}: "
                        "truncated=true ⇒ cut off at the cap, false ⇒ crashed) "
                        "so a caller need not scan by_tool[]; and "
                        "`parse_failures[]` lists source files a tool (cppcheck) "
                        "could not PARSE — a C++23 TU its frontend chokes on "
                        "gets ZERO coverage and would otherwise be silently "
                        "absent from the findings. "
                        "ANTS-3706: `parse_failures_detail[]` carries the same "
                        "set as {file, tool, reason} where reason is the "
                        "check-id + first diagnostic — so a missing include "
                        "path (fixable config) is distinguishable from a "
                        "frontend limitation (route around) without re-running "
                        "the tool by hand. `parse_failures[]` keeps its "
                        "bare-path shape. "
                        "ANTS-2183 — **long sweeps & the transport cap:** a "
                        "full-tree (scope:\"full\") run can take minutes and "
                        "exceed the MCP client's request timer (Claude "
                        "Code's is ~60 s), surfacing `MCP error -32000: "
                        "transport: timed out` *outside* the response. The "
                        "run is NOT lost — it completes server-side and "
                        "writes its SARIF; read the result back via "
                        "`last_audit_summary` (or this envelope's "
                        "`cache_path`). To stay under the cap, narrow with "
                        "`scope` / `tools` / a smaller `cap_per_tool_seconds`; "
                        "otherwise expect the `last_audit_summary` fallback "
                        "for full sweeps. ANTS-3396 — for a slow sweep, pass "
                        "`async:true` to get a job handle back instantly and "
                        "poll `audit_poll {job_id}`; the async path never "
                        "trips the transport cap at all. "
                        "See docs/specs/ANTS-1351.md + ANTS-1555.md + "
                        "ANTS-3396.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to run the static-analysis sweep without "
                        "leaving Ants. Pairs with last_audit_summary "
                        "for the compact read afterwards.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject toolsProp;
                    toolsProp["type"] = "array";
                    // ANTS-3622 — this string used to claim auto-detect
                    // excluded clang-tidy as well as mypy. It never did:
                    // kAutoDetectTools() removes only mypy, so a default
                    // sweep is 9 tools including clang-tidy. Fixed here
                    // rather than in the code because spec (ANTS-1351
                    // §2.1) and code already agreed at 9 — this string was
                    // the lone outlier, and dropping clang-tidy to match
                    // it would have silently changed what every default
                    // sweep covers. The stale clang-tidy rationale is gone
                    // too: ANTS-2182 gave it automatic compile-DB
                    // resolution, so it no longer needs a hand-passed
                    // `-p build/`.
                    toolsProp["description"] = QStringLiteral(
                        "Subset of {cppcheck, clazy, clang-tidy, ruff, "
                        "bandit, semgrep, gitleaks, trivy, shellcheck, "
                        "mypy} (10 known). Empty / omitted = auto-detect "
                        "every runnable tool EXCEPT mypy — 9 tools, "
                        "clang-tidy included. mypy runs deps-less here so "
                        "a full-sweep mypy is import-not-found noise — CI "
                        "/ pre-commit run the real deps-installed mypy "
                        "(ANTS-3418); request it explicitly to run it. "
                        "clang-tidy resolves its compile DB automatically "
                        "(ANTS-2182); pair it with `paths` + `checks` for "
                        "a scoped sweep (ANTS-1512).");
                    QJsonObject scopeProp;
                    scopeProp["type"] = "string";
                    scopeProp["description"] = QStringLiteral(
                        "Audit scope. \"auto\" (default) = full-tree "
                        "minus exclusions (some tools degrade to "
                        "changed-since-fork-point on a clean working "
                        "tree — ANTS-1456 — so a no-diff tree can read "
                        "as total_raw:0 \"clean\" when the truth is "
                        "\"nothing to audit\"). The narrowing scopes "
                        "(ANTS-1504) run each file-oriented tool against "
                        "ONLY the changed files; gitleaks/trivy skip "
                        "(reason not_file_scoped) and a tool with no "
                        "matching changed file skips "
                        "(no_changed_files_for_languages): "
                        "\"since-last-run\" = changed since the prior "
                        "cached run's commit + working tree (demotes to "
                        "a full scan with scope_demoted:\"full\" when "
                        "the prior commit is absent/unreachable); "
                        "\"files\" = changed since the merge-base with "
                        "origin/<default-branch> + working tree; "
                        "\"since-tag:<tag>\" = git diff <tag>..HEAD + "
                        "working tree (tag sanitised, INV-15); "
                        "\"branch-diff\" = git diff main..HEAD. The "
                        "envelope adds scope_resolved / "
                        "changed_files_count / scope_anchor_commit (+ "
                        "no_changes when nothing changed). Under "
                        "\"since-last-run\" it also returns a precise "
                        "delta:{added,removed,added_count,removed_count,"
                        "carried_forward_count} vs the prior run's findings "
                        "(or delta_unavailable_reason when no readable, "
                        "untruncated baseline exists) — ANTS-1870. For a "
                        "deterministic full sweep use \"full\" "
                        "(ANTS-2015): the whole tracked src/ tree, git-"
                        "diff-independent, so clazy/clang-tidy get real "
                        "source positionals instead of the empty list "
                        "\"auto\" hands them on a clean tree. No tool is "
                        "skipped under \"full\".");
                    QJsonObject capProp;
                    capProp["type"] = "integer";
                    capProp["description"] = QStringLiteral(
                        "Per-tool wall-clock cap in seconds, [5, 300]. "
                        "Caps above ~60 s are meant for the async path "
                        "(async:true), which survives the transport timeout. "
                        "Default 30. Out-of-range → bad_args.");
                    QJsonObject suppProp;
                    suppProp["type"] = "string";
                    // ANTS-3615 — describes what the headless engine actually
                    // does. `.audit_suppress` is GUI-only (line-grain dedup
                    // keys the runner never materialises); the headless
                    // sources are the learned-FP ledger + the allowlist.
                    suppProp["description"] = QStringLiteral(
                        "\"auto\" (default) applies the learned-FP ledger "
                        "(.audit_cache/learned-fp.jsonl) + "
                        ".audit_allowlist.json; \"none\" applies neither; "
                        "\"path:<file>\" reads the allowlist from the named "
                        "file instead (must resolve under the project root, "
                        "else bad_path). Any other value → bad_args.");
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
                    // ANTS-3622 — second stale string in this schema. It
                    // named cppcheck + clang-tidy only; eight of the ten
                    // tools append the scoped paths (verified against the
                    // toolArgv branches). Under-claiming here is not
                    // harmless: a caller who believes narrowing does not
                    // reach ruff/bandit/semgrep will avoid `paths` and pay
                    // for a full sweep it did not need.
                    pathsProp["description"] = QStringLiteral(
                        "Project-relative paths constraining the "
                        "tool's invocation (e.g. [\"menu/drivers/\", "
                        "\"gfx/\"]). Honoured as positional args by "
                        "every file-scoped tool: cppcheck, clazy, "
                        "clang-tidy, ruff, bandit, semgrep, shellcheck, "
                        "mypy. gitleaks and trivy are repo-global and "
                        "ignore them (they skip entirely under a "
                        "narrowing scope — not_file_scoped). Each "
                        "entry is sanitised through isAuditArgSafe; "
                        "any failing entry rejects the whole call "
                        "with code:\"bad_args\".");
                    // ANTS-3710 — the negative counterpart to `paths`.
                    // Narrowing is not excluding: `paths` also drops the
                    // repo-global tools and the project's own sibling lanes.
                    QJsonObject exclProp;
                    exclProp["type"] = "array";
                    exclProp["description"] = QStringLiteral(
                        "Project-relative path prefixes to EXCLUDE (e.g. "
                        "[\"mingw-deps\", \"third_party\"]) — for code that "
                        "is present but not yours: a vendored dependency "
                        "tree, a staged cross-build SDK, a single-header "
                        "library. Unlike `paths` (which narrows, and so also "
                        "skips gitleaks/trivy and your own sibling lanes), "
                        "this keeps the sweep whole and removes only the "
                        "named subtrees. Applied two ways: every positional "
                        "file list is prefix-filtered at a path-segment "
                        "boundary, and each tool with a native exclusion "
                        "flag gets them on its whole-tree run (cppcheck -i, "
                        "ruff --extend-exclude, bandit -x, semgrep "
                        "--exclude, trivy --skip-dirs, mypy --exclude). "
                        "gitleaks, clazy, clang-tidy and shellcheck have "
                        "neither and CANNOT honour it — when any of them ran, "
                        "the envelope's `exclude_paths_ignored_by` names "
                        "them, alongside `exclude_paths_applied`. Each entry "
                        "is sanitised through isAuditArgSafe; any failing "
                        "entry rejects the whole call with "
                        "code:\"bad_args\".");
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
                    // ANTS-3396 — opt-in async mode.
                    QJsonObject asyncProp;
                    asyncProp["type"] = "boolean";
                    asyncProp["description"] = QStringLiteral(
                        "When true, start the sweep detached and return a "
                        "job handle immediately ({async:true, job_id, "
                        "status:\"running\", poll_with:\"audit_poll\"}) "
                        "instead of blocking until it finishes. Poll "
                        "audit_poll {job_id} for completion. Default "
                        "false (synchronous, unchanged). Use it for "
                        "scope:\"full\" or slow tool sets (cold semgrep / "
                        "trivy / mypy) that can exceed the MCP client's "
                        "~60 s request timer — the async path never trips "
                        "the `transport: timed out` cap. Results are "
                        "written to .audit_cache regardless.");
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
                    props["exclude_paths"]        = exclProp;   // ANTS-3710
                    props["checks"]               = checksProp;
                    props["async"]                = asyncProp;
                    props["caller_cwd"]           = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-3396 — audit_poll (async audit_run companion).
                {
                    QJsonObject t;
                    t["name"] = "audit_poll";
                    t["description"] = QStringLiteral(
                        "Poll an async audit_run job by its job_id (from an "
                        "audit_run {async:true} call). Read-only, cheap, "
                        "never blocks. Branch on `status`, not `ok` (ok "
                        "reports whether the POLL succeeded): "
                        "\"running\" (with elapsed_ms), \"done\" (with "
                        "cache_path + total_raw/total_actionable/partial/"
                        "incomplete_tools (+ incomplete_tools_detail/"
                        "parse_failures, ANTS-3585); no_changes on an empty "
                        "changeset; "
                        "read_full_with:last_audit_summary), \"error\" (the "
                        "run's own code/error), or \"expired\" (job_id "
                        "unknown or evicted from the bounded registry — the "
                        "result is still on disk; read last_audit_summary). "
                        "Prefer the done envelope's cache_path over "
                        "last_audit_summary for this job's exact SARIF. "
                        "Required: caller_cwd. See docs/specs/ANTS-3396.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use after audit_run {async:true} to check whether "
                        "the detached sweep has finished.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject jobProp;
                    jobProp["type"] = "string";
                    jobProp["description"] = QStringLiteral(
                        "The job_id returned by audit_run {async:true} "
                        "(e.g. \"audit-7\"). Missing / non-string → "
                        "bad_args.");
                    QJsonObject callerProp;
                    callerProp["type"] = "string";
                    callerProp["description"] = QStringLiteral(
                        "Your $PWD. Required (ANTS-1404 parity with "
                        "audit_run).");
                    QJsonObject props;
                    props["job_id"]     = jobProp;
                    props["caller_cwd"] = callerProp;
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("job_id");
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
                        "Phase 1 of the test_audit trio: detect the test "
                        "framework, walk test files, pack into chunks "
                        "(size 12, [4,30]), run a pre-pass regex scan, "
                        "return paginated chunks + partition_token. "
                        "dimensions: auto (default) | csv:<d1,d2>. scope: "
                        "auto | path:<sub> | files:<csv>. Seed the subagent "
                        "with envelope-level dimensions_active[] "
                        "(auto omits 5 style dimensions — opt in via csv:; "
                        "dimensions_skipped[] says which and why). Chunks "
                        "are capped by chunk_byte_budget as well as "
                        "chunk_size. "
                        "(pre_pass_dimensions[] is a prioritisation hint, "
                        "not a coverage allow-list). Required: caller_cwd. "
                        "Pairs with test_audit_brief.");
                    // ANTS-2079 — full pagination / resume / polyglot
                    // reference in `detail` (stripped from the tools/list
                    // wire; served by tool_info
                    // {name:"test_audit_partition"}). NB: the only MCP
                    // input arg also named `detail` belongs to test_results
                    // (out of scope) — a different JSON object that never
                    // aliases this tool-descriptor field.
                    t["detail"] = QStringLiteral(
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
                        "**ANTS-2070:** the inlined "
                        "`pre_pass_findings_by_chunk` map is omitted from "
                        "the envelope when it would exceed ~24 KiB (a large "
                        "suite would otherwise overflow the tool-result "
                        "cap); the response then sets `pre_pass_omitted:true` "
                        "+ `pre_pass_omitted_bytes` and `pre_pass_cached:"
                        "true`, and the caller fetches each chunk's findings "
                        "via `test_audit_brief` (guided by "
                        "`pre_pass_chunk_ids[]`). "
                        "**ANTS-1623 polyglot:** when scope is "
                        "`path:<sub>`, the framework probe runs "
                        "inside `<sub>` FIRST (then falls back to "
                        "root) so a pytest-backend + vitest-frontend "
                        "project routes correctly to the scoped tree "
                        "instead of pinning to the root's first-"
                        "match. The envelope also carries "
                        "`additional_frameworks[]` — "
                        "`[{name, root_rel}]` pairs listing every "
                        "other framework whose signal file was "
                        "detected at the project root OR a top-level "
                        "subdir but was not picked as the primary. "
                        "Empty on single-framework projects (the "
                        "common case). Caller can fan out a second "
                        "partition call with `scope: \"path:<root_rel>\"` "
                        "to cover each extra tree. "
                        "**ANTS-1580 resume:** the returned "
                        "`partition_token` is an in-process LRU handle "
                        "(not durable on disk and not auto-saved into "
                        "session_memory). To survive a session bounce, "
                        "store it explicitly via "
                        "`session_memory(op:\"set\", "
                        "key:\"test_audit_partition_token.<scope_id>\", "
                        "value:{token, scope, dimensions, saved_at_ms})` "
                        "right after partition; resume with a matching "
                        "`op:\"get\"` and fall back to re-running this "
                        "verb if `test_audit_brief` refuses with "
                        "`partition_token not found`. Full recipe at "
                        "docs/standards/test-audit-resume.md. "
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
                    // ANTS-3630 — the `quick` property is gone. It was
                    // accepted, forwarded and never read, so `quick:true`
                    // ran an identical full walk + pre-pass. Its schema
                    // text ("skip pre-pass regex scan") also contradicted
                    // the /test-audit skill, where the grep pre-pass IS
                    // the whole audit under --quick and only the subagent
                    // phase is skipped — which is caller-side. With
                    // additionalProperties:false a stale caller now gets a
                    // loud schema refusal instead of a silent no-op.
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
                    props["chunk_size"] = cP;
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
                        "partition_token from a prior partition call. "
                        "**Rate limit (ANTS-1643):** BriefAssembly "
                        "tier — 30 calls / 60 s per (tool, "
                        "caller_cwd). A canonical `/test-audit` "
                        "Phase-2 fan-out dispatches 12-16 briefs in "
                        "one parallel batch, which fits comfortably "
                        "under this cap. Higher than the 10/min "
                        "Expensive tier the sibling `test_audit_*` "
                        "verbs sit in.");
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
                        "caller can clear a stale .lock sibling. "
                        "Each `actionable[]` item is an object with "
                        "fields: `file` (path), `line` (int), "
                        "`dimension` (one of the 18 kDimensions; 13 are "
                        "the auto default), "
                        "`severity` (CRITICAL/HIGH/MEDIUM/LOW/INFO), "
                        "`fix` (one-line remediation), and a "
                        "headline-bearing field — `headline` "
                        "preferred, with `summary` and `claim` "
                        "accepted as fallbacks (ANTS-1615). Missing "
                        "or empty in all three → "
                        "{ok:false, code:\"bad_actionable\"} naming "
                        "the offending index. Long headlines are "
                        "truncated to 120 chars with \" …\" suffix. "
                        "**Narrative mode (ANTS-1635):** pass "
                        "`narrative_mode=true` + `narrative_md=\"…\"` "
                        "to insert pre-rendered prose verbatim under "
                        "the `### 🧪 Test Audit YYYY-MM-DD` heading "
                        "instead of the per-finding bullet rendering. "
                        "Skips ID allocation and `.roadmap-counter` "
                        "touch entirely. `actionable[]` is not "
                        "required in narrative mode. Empty / "
                        "whitespace-only `narrative_md` → "
                        "{ok:false, code:\"narrative_md_required\"}.");
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
                    // ANTS-1635 — narrative-mode opt-in.
                    QJsonObject nmP; nmP["type"] = "boolean";
                    nmP["description"] = QStringLiteral(
                        "When true, skip per-finding bullet rendering "
                        "and ID allocation; insert `narrative_md` "
                        "verbatim under the section heading.");
                    QJsonObject nmdP; nmdP["type"] = "string";
                    nmdP["description"] = QStringLiteral(
                        "Pre-rendered markdown inserted under the "
                        "`### 🧪 Test Audit YYYY-MM-DD` heading when "
                        "narrative_mode=true. Required + non-empty in "
                        "that mode; ignored when narrative_mode is "
                        "false / absent. Caller owns sub-headings and "
                        "structure inside this body.");
                    QJsonObject props;
                    props["actionable"]      = aP;
                    props["framework"]       = fP;
                    props["files_scanned"]   = fsP;
                    props["dimensions"]      = dP;
                    props["raw_findings"]    = rfP;
                    props["narrative_mode"]  = nmP;
                    props["narrative_md"]    = nmdP;
                    props["caller_cwd"]      = ccwd;
                    props["dry_run"]         = makeDryRunProp();  // ANTS-2227
                    props["id_prefix"]       = makeFoldInIdPrefixProp();  // ANTS-3498
                    schema["properties"] = props;
                    // ANTS-1635 — `actionable` is no longer strictly
                    // required at the schema level; the engine refuses
                    // missing+non-narrative requests with the same
                    // `missing_field` error (now naming the
                    // narrative_mode escape hatch in the message).
                    // Keep `caller_cwd` required (mutating verb).
                    QJsonArray req;
                    req.append("caller_cwd");
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1513 — test_audit_recheck
                {
                    QJsonObject t;
                    t["name"] = "test_audit_recheck";
                    t["description"] = QStringLiteral(
                        "[test-audit] Recheck a deferred test-audit "
                        "finding's cite before resuming the work days "
                        "later. Parses ROADMAP.md for the bullet "
                        "`[<finding_id>]`, extracts the first `path:line` "
                        "citation from its body, and reports whether the "
                        "file still exists and whether the cited line "
                        "still trips any pre-pass smell pattern "
                        "(line_still_matches_pattern + matched_pattern_id "
                        "/ matched_dimension). When the file is gone, a "
                        "best-effort git rename `drift_hint` is offered "
                        "(\"file likely moved to …\"). Read-only; reads "
                        "are confined to under the project root. Returns "
                        "{ok, found, cited_file, cited_line, file_exists, "
                        "line_exists, current_line_text, "
                        "line_still_matches_pattern, drift_hint?}. "
                        "found:false when no bullet matches the id "
                        "(ANTS-1513).");
                    t["selection_hint"] = QStringLiteral(
                        "Use to recheck whether a deferred test-audit "
                        "finding's cited file:line is still valid "
                        "(file present, line still smells) before "
                        "resuming the work.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Your $PWD — the project whose ROADMAP.md "
                            "carries the finding (Required; refuses "
                            "caller_cwd_required when absent).");
                        props["caller_cwd"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "The roadmap bullet id to recheck, e.g. "
                            "\"ANTS-1234\" (matched as the `[id]` token).");
                        props["finding_id"] = p;
                    }
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append("caller_cwd");
                    req.append("finding_id");
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
                        "cited_code_paths + stale_citations + "
                        "doc_integrity + input_hash + section_index. "
                        "ANTS-3740: `section_index` is a per-doc map of the "
                        "lane's OWN docs — one entry per ATX heading with "
                        "{heading, slug, level, start_line, end_line}. Cite "
                        "findings against a section anchor rather than a line "
                        "number (an anchor survives an edit above it), and "
                        "fetch any listed section with `read_region "
                        "section=<slug>` — the slug is that verb's own key. "
                        "Capped per doc; `truncated:true` marks a doc that hit "
                        "the cap. ANTS-3718: "
                        "`input_hash` is a SHA-256 over this lane's FULL "
                        "review input — the assembled brief, the lane's "
                        "docs, the small cross-reference contracts, and "
                        "every resolved cited code file. Cache it per "
                        "lane and skip re-reviewing a lane whose hash is "
                        "unchanged since the loop it last passed clean; "
                        "the server hashes the bytes it already resolves, "
                        "so the caller never reads the input to hash it. "
                        "The large append-only logs "
                        "(large_cross_reference_docs) are excluded — they "
                        "are searched, not read, and would bust every "
                        "lane on every commit. Doc "
                        "bodies are NOT inlined (ANTS-1319 INV-3, "
                        "mirrors ANTS-1281); the subagent reads each "
                        "doc via its Read tool. The `cited_code_paths` "
                        "regex (ANTS-1633) covers `src/<path>.{h,cpp}` "
                        "(Ants-shaped) plus language-agnostic "
                        "`<path>:<line>` citations (.c/.cpp/.h/.hpp/"
                        ".py/.ts/.tsx/.js/.jsx/.go/.rs/.lua/.java/.kt/"
                        ".swift/.m/.mm/.sh) — works for non-Ants doc "
                        "lanes supplied via `doc_paths[]`. Paths the "
                        "regex matched but the filesystem could not "
                        "resolve under projectPath surface as "
                        "`stale_citations[]` — per-lane reviewers treat "
                        "non-empty entries as accuracy-dimension "
                        "findings (cited file moved/deleted). "
                        "ANTS-3522: `cited_code_regions[]` ({path, "
                        "lines[]}) carries the exact lines from "
                        "`<path>:<line>` citations so a reviewer reads a "
                        "window around each (outline + read_region) "
                        "instead of the whole file — additive alongside "
                        "cited_code_paths; empty when no line citation "
                        "resolved. "
                        "ANTS-3526: `large_cross_reference_docs[]` is the "
                        "subset of cross_reference_docs that are large "
                        "append-only logs (ROADMAP.md / CHANGELOG.md, "
                        ">100 KB); the brief routes them to a \"SEARCH, do "
                        "NOT full-read\" section so a drift-check greps the "
                        "log for the specific ID/feature/version instead of "
                        "reading tens of thousands of lines. "
                        "Required: lane (string). Optional (ANTS-1508): "
                        "doc_paths[] — when `lane` is not in the "
                        "auto-partition, the brief is synthesised "
                        "from these caller-supplied paths instead. "
                        "Paths must be project-relative and resolve "
                        "inside the project root (INV-13 enforced); "
                        "absolute paths and symlink escapes are "
                        "rejected silently. Optional (ANTS-1634b): "
                        "prior_loop_fixes[] — orchestrator-supplied "
                        "list of {title, summary} records the engine "
                        "renders as a 'Previously fixed in loop 1' "
                        "block before the Instructions section, so "
                        "loop-2 reviewers don't re-raise items "
                        "already closed. **Rate limit (ANTS-1629):** "
                        "BriefAssembly tier — 30 calls / 60 s "
                        "per (tool, caller_cwd). A canonical "
                        "`/cold-eyes` Phase-2 fan-out dispatches "
                        "12-16 briefs in one parallel batch, which "
                        "fits comfortably under this cap. Higher than "
                        "the 10/min Expensive tier the other "
                        "`cold_eyes_*` verbs sit in.");
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
                    // ANTS-1634(b) — orchestrator-supplied prior-loop
                    // fix list. Loop-2 dispatches inject one entry per
                    // item closed by loop-1 so the reviewer doesn't
                    // re-flag the same finding. Engine renders a
                    // "Previously fixed in loop 1 (do not re-raise)"
                    // section before the Instructions block.
                    QJsonObject priorFixesProp;
                    priorFixesProp["type"] = "array";
                    QJsonObject priorFixesItem;
                    priorFixesItem["type"] = "object";
                    QJsonObject priorFixesItemProps;
                    QJsonObject priorFixesTitleP;
                    priorFixesTitleP["type"] = "string";
                    priorFixesTitleP["description"] = QStringLiteral(
                        "Short headline for the closed item (e.g. "
                        "\"ANTS-1500 missing stale_reason on "
                        "counter_regressed\").");
                    QJsonObject priorFixesSummaryP;
                    priorFixesSummaryP["type"] = "string";
                    priorFixesSummaryP["description"] = QStringLiteral(
                        "One-sentence summary of what loop-1 changed. "
                        "Empty allowed but at least one of "
                        "title/summary must be non-empty (entries with "
                        "both empty are silently dropped).");
                    priorFixesItemProps["title"]   = priorFixesTitleP;
                    priorFixesItemProps["summary"] = priorFixesSummaryP;
                    priorFixesItem["properties"]   = priorFixesItemProps;
                    priorFixesItem["additionalProperties"] = false;
                    priorFixesProp["items"] = priorFixesItem;
                    priorFixesProp["description"] = QStringLiteral(
                        "Optional (ANTS-1634b). Orchestrator-supplied "
                        "list of {title, summary} records describing "
                        "items that were already fixed in loop 1. The "
                        "engine renders a 'Previously fixed in loop 1 "
                        "(do not re-raise)' section before the "
                        "reviewer Instructions block. Saves the "
                        "orchestrator from hand-rolling the block into "
                        "each subagent prompt. Empty/missing entries "
                        "are skipped; an empty array omits the "
                        "section entirely.");
                    QJsonObject props;
                    props["lane"]             = laneProp;
                    props["doc_paths"]        = docPathsProp;
                    props["prior_loop_fixes"] = priorFixesProp;
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
                        "{lane: report_text}, ANTS-1509 — **no disk "
                        "needed**, ideal for /cold-eyes which holds "
                        "agent reports inline in the orchestrator's "
                        "context rather than spilling to disk; "
                        "ANTS-1626) OR `reports_dir` (project-relative "
                        "directory of *.md files, ANTS-1282 — saves "
                        "parent context by reading from disk server-"
                        "side; ideal for /indie-review which writes "
                        "lane reports to disk). Returns findings "
                        "cited by >= min_lanes distinct reports at "
                        "the same (file, line). Pure regex pass; no "
                        "LLM. Mirrors indie_review_corroborate. "
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
                        "ANTS-1644 — pass `narrative_mode:true` + "
                        "`narrative_md:\"<pre-rendered markdown>\"` "
                        "to insert prose under the section heading "
                        "verbatim, skipping ID allocation and the "
                        "per-finding bullet rendering. Use this when "
                        "the natural shape is one prose subsection "
                        "(Closed inline / Deferred / False-positives) "
                        "rather than N bullets the reviewer doesn't "
                        "want. Required: caller_cwd (string — your "
                        "$PWD; ANTS-1372). One of: actionable "
                        "(array) OR narrative_mode=true + "
                        "narrative_md.");
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
                        "objects describing the corroborated set. "
                        "ANTS-1278 — each object MAY also carry "
                        "{title, description, layman, kind} to "
                        "render a real roadmap card (bold title + "
                        "body + Layman: + Kind:). Omit any of them "
                        "and the renderer emits a LOUD "
                        "`**TODO: describe this finding (cited by "
                        "N lanes at file:line).**` placeholder so "
                        "a caller cannot ship a stub bullet.");
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
                        "Your $PWD — the project this fold-in writes "
                        "to. Anchored to caller_cwd (ANTS-1630): the "
                        "write lands on YOUR project's ROADMAP "
                        "regardless of which Ants tab is focused; "
                        "refuses cwd_bad if it doesn't resolve to a "
                        "directory.");
                    // ANTS-1644 — narrative-mode escape hatch.
                    QJsonObject nmProp;
                    nmProp["type"] = "boolean";
                    nmProp["description"] = QStringLiteral(
                        "Opt out of per-finding bullet rendering. "
                        "When true, the handler inserts "
                        "`narrative_md` verbatim under the "
                        "`### 📝 Cold-eyes <DATE>` heading, skips "
                        "`.roadmap-counter` allocation, and returns "
                        "`allocated_ids:[]`. `actionable` is then "
                        "optional. Use for prose subsections "
                        "(Closed inline / Deferred / False-"
                        "positives) where N bullets would be "
                        "noise.");
                    QJsonObject nmdProp;
                    nmdProp["type"] = "string";
                    nmdProp["description"] = QStringLiteral(
                        "Pre-rendered markdown body for "
                        "`narrative_mode:true`. Inserted verbatim "
                        "after a blank line under the section "
                        "heading. Trailing newline added if absent. "
                        "Refused with code "
                        "`narrative_md_required` when empty or "
                        "whitespace-only.");
                    QJsonObject props;
                    props["actionable"]            = aProp;
                    props["date_iso"]              = dProp;
                    props["release_block_heading"] = hProp;
                    props["id_allocation"]         = iaProp;
                    props["narrative_mode"]        = nmProp;
                    props["narrative_md"]          = nmdProp;
                    props["dry_run"]               = makeDryRunProp();  // ANTS-2227
                    props["id_prefix"]             = makeFoldInIdPrefixProp();  // ANTS-3498
                    props["caller_cwd"]            = callerProp;
                    schema["properties"] = props;
                    // ANTS-1644 — `actionable` dropped from required.
                    // Handler still refuses with bad_args when both
                    // narrative_mode=false AND actionable[] empty.
                    QJsonArray req;
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
                        "Append or flip/annotate ROADMAP.md bullets (GFM or "
                        "Ants-v1 emoji format, auto-detected). ops: append "
                        "(default) | append_batch | flip | flip_batch | "
                        "annotate | create_section. append needs caller_cwd, "
                        "section, status, headline, kind, source; flip needs "
                        "to_status + one of (id|anchor|headline). dry_run:"
                        "true previews. Refusals: bullet_not_found, "
                        "bullet_ambiguous, anchor_unsafe_context, "
                        "bad_op_combo, bad_case, missing_field, "
                        "unrecognised_format. SIZE NOTE: keep append bodies "
                        "small — large payloads can drop in transit "
                        "(ANTS-1853); use Edit for long prose. "
                        "caller_cwd Required.");
                    // ANTS-2079 — full per-op reference in `detail`
                    // (stripped from the tools/list wire; served by
                    // tool_info {name:"roadmap_log"}).
                    t["detail"] = QStringLiteral(
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
                        "layman, lanes[], id_hint, id_prefix, dry_run. "
                        "ID prefix (ANTS-2076): counter IDs render as "
                        "[<PREFIX>-NNNN]; the prefix is sniffed from "
                        "existing IDs, else derived from caller_cwd's "
                        "leaf directory (uppercase first 4 chars, "
                        "DOOM_Ants → DOOM) — pass `id_prefix` to pin it "
                        "on a fresh roadmap. `dry_run:true` (ANTS-2077) "
                        "previews the id + bullet without writing. "
                        "Slugs / IDs are case-sensitive (off-case → "
                        "bad_case with the canonical form). ANTS-2043 — the "
                        "success envelope carries a non-blocking "
                        "`possible_duplicates:[{id, headline, score}]` "
                        "advisory (score 100 = exact normalised-headline "
                        "match, 60-99 = token overlap) when the new "
                        "headline resembles an existing bullet; the "
                        "bullet is still appended (append_batch attaches "
                        "it per accepted bullet as "
                        "`[{bullet_index, id, candidates[]}]`). "
                        "op:\"flip\" (ANTS-1428) — flips a bullet's "
                        "status; injects an Obsidian-style "
                        "`^prefix-NNNN` anchor on first touch as the "
                        "durable handle. Required: caller_cwd, "
                        "to_status, and one of (id | anchor | "
                        "headline). Optional: prefix_hint, and `note` "
                        "to append a resolution line while flipping in "
                        "one call (ANTS-1793). "
                        "op:\"annotate\" (ANTS-1717) — appends `note` "
                        "to a located bullet's body and leaves status "
                        "unchanged (no flip, no anchor); for recording "
                        "partial progress on a still-open item. "
                        "Required: caller_cwd, note, and one of (id | "
                        "anchor | headline); rejects to_status with "
                        "bad_op_combo. Refusal codes: "
                        "bullet_not_found, bullet_ambiguous, "
                        "anchor_unsafe_context, bad_op_combo, "
                        "missing_field, unrecognised_format. Returns "
                        "{ok, id?, file, line, bytes_written, "
                        "note_appended?, note_line?, ...op-specific} "
                        "or {ok:false, error, code}. "
                        "NESTED SUB-BULLETS (ANTS-3403): flip/flip_batch/"
                        "annotate locators resolve TOP-LEVEL bullets only. "
                        "A multi-phase epic that tracks phases as nested "
                        "list items inside a narrator bullet's body "
                        "(`  - ✅ **Phase B — …**`) is out of scope — the "
                        "walker does not descend into a bullet's body, so a "
                        "per-phase flip returns bullet_not_found. Edit "
                        "ROADMAP.md directly to tick a nested phase line. "
                        "PASS-HEADINGS (ANTS-2126): on a `#### Pass N.M` "
                        "heading roadmap (RetroDB-style; reader synthesises "
                        "`PASS-N-M[-SUB]` ids), append/append_batch/flip/"
                        "flip_batch/annotate now WRITE instead of refusing "
                        "format_mismatch (only create_section still "
                        "refuses). op:\"append\" needs a `pass` arg "
                        "(e.g. \"43.5\" or \"43.5.B\", validated "
                        "^\\d+\\.\\d+(?:\\.[A-Za-z][A-Za-z0-9]*)?$); status "
                        "is required, kind/source/lanes/layman are ignored, "
                        "and .roadmap-counter is never touched. Flip/annotate "
                        "locate by the `PASS-N-M` id (from roadmap_query) or "
                        "`headline`; a missing required arg is `bad_args`. A "
                        "stray `pass` on a GFM/ants-v1 roadmap is ignored. "
                        "SIZE NOTE (ANTS-1853): keep op:\"append\" "
                        "calls small. Large multi-paragraph `body` "
                        "payloads (many embedded newlines/quotes) are "
                        "intermittently dropped in transit by the "
                        "tool-call transport — the arguments arrive "
                        "empty and the call refuses with "
                        "arguments_empty (NOT an Ants bug; the data "
                        "never reaches the server). Small bodies are "
                        "reliable. For a long entry, prefer a concise "
                        "`body` (≲1–2 short paragraphs) or write the "
                        "prose with the Edit tool directly into "
                        "ROADMAP.md. If a call refuses with "
                        "arguments_empty, just resend it.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to append a new bullet or flip an "
                        "existing one's status on ROADMAP.md. "
                        "Mutates project state — caller_cwd "
                        "required. Keep append bodies small — large "
                        "payloads can be dropped in transit "
                        "(ANTS-1853); use Edit for long prose.");
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
                        // ANTS-4385 — this used to say "from `roadmap_query`'s
                        // section echo". No field or mode is called that, so a
                        // caller searching the schema for "echo" found nothing
                        // and concluded the route did not exist — which is
                        // exactly what happened, costing one false finding and
                        // its retraction. Name the mode.
                        "Slug of a ## or ### heading "
                        "(e.g. \"performance-2\"). Get valid slugs from "
                        "`roadmap_query` mode:\"section_index\" (or its alias "
                        "mode:\"sections\"). New "
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

                    // ANTS-3382 — optional evidence file paths for
                    // image/log-driven bullets (e.g. a bug diagnosed from
                    // screenshots). Emitted as an `Evidence: <paths>` line
                    // and echoed back by roadmap_query as an `evidence`
                    // array. Accepted on op:"append" / "append_batch".
                    QJsonObject evidenceProp;
                    evidenceProp["type"] = "array";
                    QJsonObject evidenceItem;
                    evidenceItem["type"]      = "string";
                    evidenceItem["maxLength"] = 500;
                    evidenceProp["items"] = evidenceItem;
                    evidenceProp["description"] = QStringLiteral(
                        "Optional file paths (screenshots, logs) that "
                        "evidence this item. Emitted as an `Evidence: "
                        "path1, path2` line and echoed by roadmap_query "
                        "as an `evidence` array so a later session can "
                        "re-locate the files. Commas/newlines inside a "
                        "path are folded to spaces.");

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
                    opEnum.append("append_batch");
                    opEnum.append("flip");
                    opEnum.append("flip_batch");
                    opEnum.append("annotate");
                    opEnum.append("amend_body");   // ANTS-3406
                    opEnum.append("amend_headline");  // ANTS-4372
                    opEnum.append("create_section");
                    opEnum.append("bundle_row");
                    opProp["enum"] = opEnum;
                    opProp["description"] = QStringLiteral(
                        "Verb mode. Default \"append\" (ANTS-1424). "
                        "\"append_batch\" (ANTS-1879) appends N bullets "
                        "to one `section` in a single read + single "
                        "commit — pass `bullets[]` (each with the same "
                        "fields as op:\"append\"). Per-bullet validation "
                        "failures land in `skipped[]` while accepted "
                        "bullets still apply (semantic parity with "
                        "flip_batch). \"flip\" routes to the status-flip "
                        "path (ANTS-1428; works on GFM-task-list and "
                        "Ants-v1 emoji formats — ANTS-1441) and accepts "
                        "an optional `note` to append a resolution line "
                        "in the same call (ANTS-1793). \"flip_batch\" "
                        "(ANTS-1690) flips N bullets to one `to_status` "
                        "in a single read + single atomic commit — pass "
                        "`locators[]` (each {id|anchor|headline|"
                        "line_range} + optional per-locator `note` and "
                        "`no_anchor`); an unresolvable locator lands in "
                        "the response `skipped[]` while the rest apply. "
                        "Use it to close a whole bundle without N "
                        "round-trips or racing the file watcher. "
                        "\"annotate\" "
                        "(ANTS-1717) appends a `note` to a located "
                        "bullet WITHOUT changing its status — for "
                        "recording partial progress on a still-open "
                        "item; no status flip, no anchor injection. "
                        "\"create_section\" (ANTS-1878) splices a new "
                        "## / ### heading after an existing section — "
                        "pass `after_section` (slug), `level` (2 or 3), "
                        "`title`, optional `intro_body`. Returns the "
                        "new slug so a follow-up op:\"append\" / "
                        "op:\"append_batch\" can key on it. "
                        "\"bundle_row\" (ANTS-1691) appends a row to a "
                        "Markdown progress table under a section heading — "
                        "pass `section` (heading text/slug) + `cells` "
                        "(non-empty array of cell strings), optional "
                        "`header` (creates the table if absent), `position` "
                        "(\"end\" default) and `sort_col` (keep the table "
                        "sorted by a column). Pipe-escapes each cell and "
                        "folds newlines to <br>, so a bundle-tracking table "
                        "no longer needs a hand-edit / sed. "
                        "\"amend_body\" (ANTS-3406) patches a bullet's "
                        "continuation prose in place — locate by "
                        "id/anchor/headline, then replace the EXACT "
                        "single-line `old_text` with `new_text` (unique-match "
                        "guarded; status/headline out of scope; dry_run "
                        "previewable). Use it to fix a stale phrase inside an "
                        "existing bullet's body without a raw text edit.");
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

                    // ANTS-1717/1793 — note to append to the located
                    // bullet's body. Required under op:\"annotate\";
                    // optional under op:\"flip\".
                    QJsonObject noteProp;
                    noteProp["type"]      = "string";
                    noteProp["maxLength"] = 4000;
                    noteProp["description"] = QStringLiteral(
                        "Prose appended as indented continuation "
                        "line(s) at the end of the located bullet's "
                        "body — the standard \"Resolved (date): …\" / "
                        "\"Progress (date): …\" close-or-update line. "
                        "Required under op:\"annotate\" (status "
                        "untouched); optional under op:\"flip\" (append "
                        "the resolution note while flipping in one "
                        "call). Scrubbed of leaked tool-call XML like "
                        "op:\"append\"'s body; pre-wrap to ~70 columns.");

                    // ANTS-3406 — op:"amend_body" operands: replace an
                    // exact single-line substring of the located bullet's
                    // continuation body.
                    QJsonObject oldTextProp;
                    oldTextProp["type"]      = "string";
                    oldTextProp["maxLength"] = 4000;
                    oldTextProp["description"] = QStringLiteral(
                        "op:\"amend_body\" (ANTS-3406) / "
                        "op:\"amend_headline\" (ANTS-4372) — the EXACT substring "
                        "to replace inside the located bullet's continuation "
                        "body. Must occur on exactly one body line (0 → "
                        "body_match_not_found, >1 → body_match_ambiguous, so "
                        "it can't silently clobber unrelated prose). "
                        "Case-sensitive; single-line (a phrase spanning a "
                        "line break won't match). The headline is out of "
                        "scope — amend_body edits body prose only. "
                        "op:\"amend_headline\" is the mirror: it edits the "
                        "HEADLINE text only, with the body out of scope. A "
                        "headline is not immutable metadata (a phase prefix, "
                        "a typo, a renamed subsystem), and without this op a "
                        "four-word change forced a native Read of the whole "
                        "ROADMAP.md — file_outline / read_region do NOT "
                        "satisfy the native Edit tool's read-precondition. It "
                        "refuses bad_args on a `new_text` that would alter the "
                        "`- <emoji> [ID] **` prefix or the bold delimiters, "
                        "which makes it strictly safer than the hand-edit it "
                        "replaces; refusals are headline_match_not_found / "
                        "headline_match_ambiguous. NOT supported on a migrated "
                        "project: the headline is a store column and its "
                        "locate key, so a markdown-only patch would be "
                        "reverted by the next render. "
                        "ANTS-4097: single-line is a MATCHING rule, not a "
                        "safety one — rewriting a phrase that spans a "
                        "hard-wrapped paragraph takes N calls, each succeeds, "
                        "each looks right alone, and the paragraph they "
                        "jointly produce is checked by nothing. The success "
                        "envelope echoes `body_paragraph` (the edited line "
                        "with its wrapped neighbours) so you can read the "
                        "joint result without re-reading the file.");
                    QJsonObject newTextProp;
                    newTextProp["type"]      = "string";
                    newTextProp["maxLength"] = 4000;
                    newTextProp["description"] = QStringLiteral(
                        "op:\"amend_body\" (ANTS-3406) — replacement text for "
                        "the unique `old_text` match. Required (the key must "
                        "be present); an empty string deletes the matched "
                        "phrase. Scrubbed of leaked tool-call XML like a "
                        "`note`.");

                    // ANTS-1690 — flip_batch locators array. Each item
                    // carries one locator (id|anchor|headline|line_range)
                    // plus optional per-locator note + no_anchor opt-out.
                    QJsonObject locItem;
                    locItem["type"] = "object";
                    QJsonObject locItemProps;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Bold-ID / [PREFIX-NNNN] locator for this bullet.");
                        locItemProps["id"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Caret-anchor locator (GFM only).");
                        locItemProps["anchor"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Headline locator (hash-matched, like op:\"flip\").");
                        locItemProps["headline"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "array";
                        QJsonObject ip; ip["type"] = "integer";
                        p["items"] = ip;
                        p["minItems"] = 2;
                        p["maxItems"] = 2;
                        p["description"] = QStringLiteral(
                            "[start,end] 1-based line range; flips every "
                            "bullet whose line falls inside it.");
                        locItemProps["line_range"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["maxLength"] = 4000;
                        p["description"] = QStringLiteral(
                            "Optional per-locator resolution note appended "
                            "to this bullet's body (scrubbed like a flip "
                            "note). Lets each bundle bullet carry its own "
                            "closure line.");
                        locItemProps["note"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "boolean";
                        p["description"] = QStringLiteral(
                            "If true, do NOT inject a caret anchor on a GFM "
                            "bullet that lacks an id/anchor (keeps a "
                            "narrator-format section anchor-free).");
                        locItemProps["no_anchor"] = p;
                    }
                    locItem["properties"] = locItemProps;
                    QJsonObject locatorsProp;
                    locatorsProp["type"]  = "array";
                    locatorsProp["items"] = locItem;
                    locatorsProp["description"] = QStringLiteral(
                        "Required under op:\"flip_batch\": the bullets to "
                        "flip to `to_status`. Each is one locator "
                        "(id|anchor|headline|line_range) + optional `note` "
                        "and `no_anchor`. Single read + single atomic "
                        "commit across all of them.");

                    // ANTS-1879 — bullets[] array for op:"append_batch".
                    // Each element carries the same shape as the
                    // single-bullet path's top-level fields.
                    QJsonObject bulletItem;
                    bulletItem["type"] = "object";
                    QJsonObject bulletItemProps;
                    {
                        QJsonObject p = headlineProp;
                        bulletItemProps["headline"] = p;
                    }
                    {
                        QJsonObject p = statusProp;
                        bulletItemProps["status"] = p;
                    }
                    {
                        QJsonObject p = kindProp;
                        bulletItemProps["kind"] = p;
                    }
                    {
                        QJsonObject p = sourceProp;
                        bulletItemProps["source"] = p;
                    }
                    {
                        QJsonObject p = bodyProp;
                        bulletItemProps["body"] = p;
                    }
                    {
                        QJsonObject p = laymanProp;
                        bulletItemProps["layman"] = p;
                    }
                    {
                        QJsonObject p = lanesProp;
                        bulletItemProps["lanes"] = p;
                    }
                    {
                        QJsonObject p = evidenceProp;  // ANTS-3382
                        bulletItemProps["evidence"] = p;
                    }
                    {
                        QJsonObject p = idHintProp;
                        p["description"] = QStringLiteral(
                            "Optional explicit ID under op:\"append_batch\". "
                            "Honoured ONLY on the first bullet (later "
                            "bullets follow first_id+i to keep the "
                            "allocation contiguous). Counter strategy only.");
                        bulletItemProps["id_hint"] = p;
                    }
                    {
                        // ANTS-2078 — per-bullet stable id. Required on
                        // every bullet when the batch-wide id_strategy is
                        // "stable_prefix"; the counter is skipped and this
                        // string is written verbatim.
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Full stable id string for THIS bullet when "
                            "the top-level id_strategy=\"stable_prefix\" "
                            "(e.g. \"Ts20-SP6\"). Must match "
                            "^[A-Za-z][A-Za-z0-9_-]+$ and be unique within "
                            "the batch. Ignored under the counter strategy "
                            "(ANTS-2078).");
                        bulletItemProps["stable_id"] = p;
                    }
                    {   // ANTS-4354 — per-bullet pass designator.
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Pass designator for THIS bullet on a "
                            "pass-headings roadmap (e.g. \"43.5\"), mirroring "
                            "`stable_id`. This is what lets a batch of N "
                            "passes name N designators rather than sharing "
                            "one; the call-level `pass` is the fallback for "
                            "any bullet omitting it. Ignored on GFM / ants-v1 "
                            "roadmaps.");
                        bulletItemProps["pass"] = p;
                    }
                    bulletItem["properties"] = bulletItemProps;
                    QJsonObject bulletsProp;
                    bulletsProp["type"]  = "array";
                    bulletsProp["items"] = bulletItem;
                    bulletsProp["description"] = QStringLiteral(
                        "Required under op:\"append_batch\": the bullets "
                        "to append to `section`. Single read + single "
                        "atomic commit across all of them. Per-bullet "
                        "validation failures land in `skipped[]` while "
                        "accepted bullets still apply (semantic parity "
                        "with flip_batch).");

                    // ANTS-1878 — create_section params.
                    QJsonObject afterSectionProp;
                    afterSectionProp["type"] = "string";
                    afterSectionProp["description"] = QStringLiteral(
                        "Required under op:\"create_section\": slug of "
                        "an existing ## / ### heading. The new heading "
                        "is inserted at this section's end.");
                    QJsonObject levelProp;
                    levelProp["type"] = "integer";
                    QJsonArray levelEnum;
                    levelEnum.append(2);
                    levelEnum.append(3);
                    levelProp["enum"] = levelEnum;
                    levelProp["description"] = QStringLiteral(
                        "Required under op:\"create_section\": heading "
                        "depth (2 for `##`, 3 for `###`).");
                    QJsonObject titleProp;
                    titleProp["type"] = "string";
                    titleProp["maxLength"] = 200;
                    titleProp["description"] = QStringLiteral(
                        "Required under op:\"create_section\": literal "
                        "heading text. The slug is computed from it via "
                        "the same transform RoadmapIndex uses.");
                    QJsonObject introBodyProp;
                    introBodyProp["type"]      = "string";
                    introBodyProp["maxLength"] = 4000;
                    introBodyProp["description"] = QStringLiteral(
                        "Optional under op:\"create_section\": intro "
                        "paragraph(s) for the new section. Single "
                        "newlines = paragraph breaks; hard-wrapped at "
                        "80 cols on word boundaries. Lines matching "
                        "^#{1,6}\\s (a Markdown heading) refuse with "
                        "bad_intro.");

                    // ANTS-1905 — id_strategy + stable_id for projects
                    // that use stable string IDs (Sh4, Ts20-FL1, MT8…)
                    // instead of [PROJ-NNNN]. Optional escape hatch
                    // around .roadmap-counter for op:"append" so a
                    // project that never had a counter can still drive
                    // the verb from MCP instead of falling back to Edit.
                    QJsonObject idStrategyProp;
                    idStrategyProp["type"] = "string";
                    {
                        QJsonArray e;
                        e.append("counter");
                        e.append("stable_prefix");
                        idStrategyProp["enum"] = e;
                    }
                    idStrategyProp["description"] = QStringLiteral(
                        "Allocator strategy under op:\"append\" / "
                        "\"append_batch\". \"counter\" (default) bumps "
                        ".roadmap-counter and renders an [ANTS-NNNN]-style "
                        "id. \"stable_prefix\" requires `stable_id` "
                        "(op:\"append\") or a per-bullet `stable_id` on "
                        "EVERY bullet (op:\"append_batch\", ANTS-2078) and "
                        "writes each bullet with that id verbatim — skip "
                        "the counter for projects that use stable string "
                        "IDs (Sh4, Ts20-FL1, MT8…) (ANTS-1905). When "
                        ".roadmap-counter is missing and stable IDs are "
                        "detected, the counter path's refusal envelope "
                        "(`stable_prefix_unsupported`) points the caller "
                        "at this strategy.");
                    QJsonObject stableIdProp;
                    stableIdProp["type"] = "string";
                    stableIdProp["description"] = QStringLiteral(
                        "Full stable id string when id_strategy="
                        "\"stable_prefix\" (e.g. \"Ts20-SP6\"). Must "
                        "match ^[A-Za-z][A-Za-z0-9_-]+$. Not accepted "
                        "under the default counter strategy.");

                    // ANTS-2076 — explicit counter-ID prefix override.
                    QJsonObject idPrefixProp;
                    idPrefixProp["type"]    = "string";
                    // ANTS-3492 — prefix may be digit-led if letter-containing.
                    idPrefixProp["pattern"] =
                        "^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]{0,15}$";
                    idPrefixProp["description"] = QStringLiteral(
                        "Optional counter-ID prefix for op:\"append\" / "
                        "\"append_batch\" (counter strategy). Overrides "
                        "BOTH the prefix sniffed from existing IDs and "
                        "the project-dir default. Use it on a fresh / "
                        "id-less roadmap to pin the project's prefix "
                        "(e.g. \"DOOM\" → DOOM-0001). When omitted, the "
                        "prefix is sniffed from existing IDs, else "
                        "derived from caller_cwd's leaf directory "
                        "(uppercase first 4 chars). Ignored under "
                        "id_strategy:\"stable_prefix\".");

                    // ANTS-2077 — dry_run preview flag. ANTS-2136 extended it
                    // to every other op and ANTS-3798 to the last one
                    // (bundle_row); the description below said "append /
                    // append_batch" until then, which is how bundle_row's
                    // missing preview stayed invisible — a reader checking
                    // whether their op supported it found a doc implying none
                    // did except append. It now enumerates, so the next op
                    // added is either listed here or visibly absent.
                    QJsonObject dryRunProp;
                    dryRunProp["type"] = "boolean";
                    dryRunProp["description"] = QStringLiteral(
                        "Optional. Supported on EVERY op — \"append\" and "
                        "\"append_batch\" (ANTS-2077), \"flip\", "
                        "\"flip_batch\", \"annotate\", \"create_section\" and "
                        "\"amend_body\" / \"amend_headline\" (ANTS-2136, "
                        "ANTS-4372), \"bundle_row\" "
                        "(ANTS-3798). Returns the resolved preview WITHOUT "
                        "writing ROADMAP.md or bumping .roadmap-counter — a "
                        "free pre-flight to verify prefix / format / section / "
                        "locator before committing. Envelope carries "
                        "dry_run:true and reports `bytes` (would-be) in place "
                        "of bytes_written; append/append_batch add the "
                        "would-be id(s), formatted bullet(s) and 1-based "
                        "insertion line(s), append_batch reporting "
                        "applied_count:0 + would_apply_count; bundle_row adds "
                        "the rendered `row` plus the row_index / columns / "
                        "created_table the write would have produced.");

                    // ANTS-2126 — pass designator for op:"append" on a
                    // `#### Pass N.M` heading roadmap.
                    QJsonObject passProp;
                    passProp["type"] = "string";
                    passProp["description"] = QStringLiteral(
                        "Pass designator for op:\"append\" / "
                        "op:\"append_batch\" on a pass-headings "
                        "(`#### Pass N.M`) roadmap (ANTS-2126), e.g. \"43.5\" "
                        "or \"43.5.B\"; validated "
                        "^\\d+\\.\\d+(?:\\.[A-Za-z][A-Za-z0-9]*)?$. REQUIRED "
                        "when the target is a pass-headings roadmap; ignored "
                        "on GFM / ants-v1 roadmaps. Flip/annotate locate a "
                        "pass by its synthesised `PASS-N-M` id (via the `id` "
                        "locator) or `headline`, not via `pass`. "
                        "ANTS-4354 — under op:\"append_batch\" a designator "
                        "may be set PER BULLET (a `pass` on the bullets[] "
                        "item, mirroring `stable_id`), so a batch of N passes "
                        "can name N designators; this call-level value is the "
                        "FALLBACK for any bullet that carries none. Previously "
                        "the batch read neither this nor an item `pass` from "
                        "the schema's point of view, so every bullet refused "
                        "bad_args and the op could write nothing at all on "
                        "this dialect. "
                        "ANTS-4117 — what append RENDERS on this format is "
                        "fixed: `#### Pass <pass> <headline>` + `- "
                        "**Status**: <keyword>` + `body` VERBATIM (not "
                        "indented for you), where <keyword> is the canonical "
                        "todo / in-progress / done / deferred, NOT the "
                        "`status` word you passed; `kind` / `source` / "
                        "`lanes` / `layman` have no slot and are ignored. A "
                        "trailing `---` is emitted only when the file already "
                        "separates its pass blocks that way. Pass "
                        "`dry_run:true` (or read the `bullet` field the write "
                        "envelope echoes) to see the exact block.");

                    // ANTS-2080 — confirm-after compact echo.
                    QJsonObject returnProp;
                    returnProp["type"] = "string";
                    {
                        QJsonArray e;
                        e.append("default");
                        e.append("headline_only");
                        returnProp["enum"] = e;
                    }
                    returnProp["description"] = QStringLiteral(
                        "Optional. \"headline_only\" on op:\"append\" / "
                        "\"append_batch\" / \"flip\" / \"flip_batch\" adds "
                        "`post_bullets` to the success envelope — the "
                        "just-touched bullet(s) in the compact {id, status, "
                        "headline_oneline} shape roadmap_query "
                        "mode:\"headline_only\" emits — so a confirm-after "
                        "read folds into the write (no follow-up "
                        "roadmap_query). \"default\" (omitted) keeps the "
                        "lean envelope (ANTS-2080 append; ANTS-2089 flip).");

                    // ANTS-3432 — op:"bundle_row" params. The handler
                    // (cmdRoadmapLogBundleRow) has always read these, but
                    // they were never declared here, so with
                    // additionalProperties:false the client stripped them
                    // and `cells` reached the handler empty → missing_field.
                    // Declaring them completes the ANTS-1691 wiring.
                    QJsonObject cellsProp;
                    cellsProp["type"] = "array";
                    {
                        QJsonObject item;
                        item["type"] = "string";
                        cellsProp["items"] = item;
                    }
                    cellsProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — the row's cells, one string per "
                        "column (non-empty). Must match the table's column "
                        "count (or `header`'s length when creating the "
                        "table), else column_mismatch. Each cell is "
                        "pipe-escaped and its newlines folded to <br> so a "
                        "`|` inside a cell can't corrupt the column count.");

                    QJsonObject headerProp;
                    headerProp["type"] = "array";
                    {
                        QJsonObject item;
                        item["type"] = "string";
                        headerProp["items"] = item;
                    }
                    headerProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — optional column names. When the "
                        "section has no Markdown table yet, `header` creates "
                        "one (header row + separator + the first data row); "
                        "when a table exists it is validated against the "
                        "column count. Omit to append to an existing table.");

                    QJsonObject positionProp;
                    positionProp["type"] = "string";
                    {
                        QJsonArray e;
                        e.append("end");
                        e.append("sorted");
                        positionProp["enum"] = e;
                    }
                    positionProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — where to insert the new row. "
                        "\"end\" (default) appends after the last data row; "
                        "\"sorted\" inserts in ascending order by `sort_col` "
                        "(numeric-aware collation) to keep the table sorted.");

                    QJsonObject sortColProp;
                    sortColProp["type"]    = "integer";
                    sortColProp["minimum"] = 0;
                    sortColProp["description"] = QStringLiteral(
                        "op:\"bundle_row\" — 0-based column index used when "
                        "`position` is \"sorted\". Ignored otherwise.");

                    QJsonObject props;
                    props["caller_cwd"]    = callerProp;
                    props["op"]            = opProp;
                    props["cells"]         = cellsProp;     // ANTS-3432
                    props["header"]        = headerProp;    // ANTS-3432
                    props["position"]      = positionProp;  // ANTS-3432
                    props["sort_col"]      = sortColProp;   // ANTS-3432
                    props["locators"]      = locatorsProp;
                    props["bullets"]       = bulletsProp;
                    props["section"]       = sectionProp;
                    props["after_section"] = afterSectionProp;
                    props["level"]         = levelProp;
                    props["title"]         = titleProp;
                    props["intro_body"]    = introBodyProp;
                    props["status"]        = statusProp;
                    props["to_status"]     = toStatusProp;
                    props["headline"]      = headlineProp;
                    props["kind"]          = kindProp;
                    props["source"]        = sourceProp;
                    props["body"]          = bodyProp;
                    props["layman"]        = laymanProp;
                    props["lanes"]         = lanesProp;
                    props["evidence"]      = evidenceProp;   // ANTS-3382
                    props["id_hint"]       = idHintProp;
                    props["id"]            = idProp;
                    props["anchor"]        = anchorProp;
                    props["prefix_hint"]   = prefixHintProp;
                    props["note"]          = noteProp;
                    props["old_text"]      = oldTextProp;     // ANTS-3406
                    props["new_text"]      = newTextProp;     // ANTS-3406
                    props["id_strategy"]   = idStrategyProp;  // ANTS-1905
                    props["stable_id"]     = stableIdProp;    // ANTS-1905
                    props["id_prefix"]     = idPrefixProp;    // ANTS-2076
                    props["dry_run"]       = dryRunProp;      // ANTS-2077
                    props["return"]        = returnProp;      // ANTS-2080
                    props["pass"]          = passProp;        // ANTS-2126
                    schema["properties"]   = props;

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
                // ANTS-3855 — roadmap_migrate: load one project's markdown
                // roadmap into the roadmap store. The only production entry
                // point into the migration engine (read half ANTS-3757, load
                // half ANTS-3765), so Required-contract gated like the other
                // whole-project write verbs.
                {
                    QJsonObject t;
                    t["name"] = "roadmap_migrate";
                    t["description"] = QStringLiteral(
                        "Load ONE project's markdown roadmap (ROADMAP.md + any "
                        "rotated archives under docs/roadmap/) into the roadmap "
                        "store, resolved from caller_cwd. One project per call. "
                        "`dry_run:true` plans every write, reports the counts and "
                        "rolls back — note it still OPENS the store, and creates "
                        "an EMPTY schema if none exists, because its counts are a "
                        "diff against existing rows. Optional project_name / "
                        "export_slug default from caller_cwd's leaf directory "
                        "(export_slug slugified); a supplied export_slug is "
                        "validated verbatim, never rewritten, and must match "
                        "[a-z0-9][a-z0-9-]*. Re-running over an unchanged project "
                        "is idempotent; re-running with a different name or slug "
                        "is refused rather than silently applied. Refusals: "
                        "no_project, no_roadmap, case_ambiguous, not_utf8, "
                        "format_mismatch, bad_args, slug_collision, store_failed, "
                        "migrate_failed. caller_cwd Required.");
                    // ANTS-1453 — selection_hint: one sentence on WHEN to
                    // reach for this verb rather than what it does.
                    t["selection_hint"] = QStringLiteral(
                        "Use ONCE per project to move it off hand-edited "
                        "markdown onto the roadmap store — roadmap_query / "
                        "roadmap_log then serve it from the store, no restart. "
                        "Run dry_run:true first for the counts and notes.");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject props;
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Your $PWD. REQUIRED — the project root to migrate.");
                        props["caller_cwd"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Optional display name stored as project.name. "
                            "Defaults to caller_cwd's leaf directory verbatim. "
                            "Must be non-empty after trimming.");
                        props["project_name"] = p;
                    }
                    {
                        QJsonObject p;
                        p["type"] = "string";
                        p["description"] = QStringLiteral(
                            "Optional export key stored as project.export_slug "
                            "(UNIQUE across the store). Defaults to the "
                            "slugified leaf directory (Ants_Terminal → "
                            "ants-terminal). A SUPPLIED value is validated "
                            "verbatim and never slugified for you: it must start "
                            "with [a-z0-9] and contain only [a-z0-9-], else "
                            "bad_args.");
                        props["export_slug"] = p;
                    }
                    props["dry_run"] = makeDryRunProp();
                    schema["properties"] = props;
                    QJsonArray req;
                    req.append(QStringLiteral("caller_cwd"));
                    schema["required"] = req;
                    schema["additionalProperties"] = false;
                    t["inputSchema"] = schema;
                    tools.append(t);
                }
                // ANTS-1548 — changelog_log: token-frugal CHANGELOG writer.
                {
                    QJsonObject t;
                    t["name"] = "changelog_log";
                    t["description"] = QStringLiteral(
                        "Append a Keep-a-Changelog entry under ## "
                        "[Unreleased] in CHANGELOG.md without re-emitting "
                        "the file. ops: add (default) | add_from_roadmap "
                        "(cite a ROADMAP id; reuses its headline + Layman "
                        "line) | add_batch (entries[], one atomic commit) | "
                        "normalize (reorder [Unreleased]'s ### category "
                        "blocks into canonical order + fold stray prose "
                        "into the bullet above it). "
                        "Category from `category` or derived from `kind`. "
                        "Required: caller_cwd (+ summary for add, id for "
                        "add_from_roadmap, entries[] for add_batch). "
                        "dry_run:true previews without writing. "
                        "Refusals: not_unreleased, bad_category, "
                        "no_changelog, format_mismatch, id_not_in_roadmap, "
                        "feature_grouped_section, missing_field, bad_args, "
                        "bad_op_combo.");
                    // ANTS-2079 — full per-op reference in `detail`
                    // (stripped from the tools/list wire; served by
                    // tool_info {name:"changelog_log"}).
                    t["detail"] = QStringLiteral(
                        "Append a Keep-a-Changelog entry under the "
                        "`## [Unreleased]` section of CHANGELOG.md "
                        "without re-emitting the file. Mode picked by "
                        "`op` (default \"add\"). op:\"add\" — render a "
                        "bullet `- **<summary>** (<id>)` (+ optional "
                        "`body` as indented continuation) under the "
                        "`### <category>` heading; the category is taken "
                        "from `category` or derived from `kind` "
                        "(fix/*-fix → Fixed, feature/implement/"
                        "enhancement → Added, security → Security, else "
                        "Changed). The `### <category>` heading is "
                        "created in canonical order if absent. "
                        "op:\"add_from_roadmap\" — cite a ROADMAP bullet "
                        "by `id`; its headline becomes the summary and "
                        "its `Layman:` line the body (reused verbatim, "
                        "not regenerated — keeps CHANGELOG + ROADMAP in "
                        "lockstep), category derived from the bullet's "
                        "`Kind:`. A wrapped multi-line headline is "
                        "collapsed to a single line (ANTS-1868) so the "
                        "rendered bullet stays a well-formed Markdown "
                        "list item. New bullets insert at the TOP of "
                        "their category (most-recent-first). "
                        "op:\"add_batch\" (ANTS-2044) — write N "
                        "`entries[]` in one read + one atomic commit; "
                        "each entry auto-detects add vs add_from_roadmap, "
                        "applies in input order (byte-identical to the "
                        "same N sequential calls), and per-entry failures "
                        "land in `skipped[]:[{index, code, error}]` while "
                        "the rest apply. "
                        "op:\"normalize\" (ANTS-3495) — reorder the "
                        "`### <category>` blocks under `## [Unreleased]` "
                        "into canonical Keep-a-Changelog order "
                        "(Added/Changed/Deprecated/Removed/Fixed/Security). "
                        "Non-destructive: each block's bullets and any "
                        "wedged prose move with its heading; a duplicate "
                        "or non-canonical heading keeps its relative "
                        "position; a preamble paragraph above the first "
                        "`### ` is untouched. No summary/body/id needed. "
                        "ANTS-3381 adds the other half of the ANTS-2125 "
                        "advisory in the same atomic write: a stray "
                        "flush-left prose line wedged between blocks is "
                        "FOLDED into the nearest preceding bullet as a "
                        "two-space continuation, reported line by line in "
                        "`moves[]:[{from_line, under_line, text}]` with a "
                        "`prose_moved` count. Preview it with dry_run "
                        "before writing — the failure mode this policy "
                        "accepts is a paragraph meant to stand alone being "
                        "absorbed by the entry above it. Prose separated "
                        "from its bullet by a heading is left alone and "
                        "keeps raising the `advisory`. "
                        "Returns {ok, op, file, changed, order_before, "
                        "order_after, prose_moved, moves?, bytes_written, "
                        "file_bytes} "
                        "(ANTS-3723: bytes_written is the ADDED-bytes delta, "
                        "matching roadmap_log — 0 for a pure reorder; "
                        "file_bytes is the whole file) (bytes + dry_run:true "
                        "under preview; no write when already canonical, "
                        "changed:false). Required: "
                        "caller_cwd (+ summary for add, or id for "
                        "add_from_roadmap, or entries[] for add_batch). "
                        "Atomic via QSaveFile. "
                        "Refusals: not_unreleased (no `## [Unreleased]` "
                        "heading), bad_category, no_changelog, "
                        "format_mismatch (YAML changelog), "
                        "id_not_in_roadmap, feature_grouped_section "
                        "(normalize: dated `### ` topics, not flat "
                        "categories), missing_field, bad_args "
                        "(empty entries[]), bad_op_combo. "
                        "Returns {ok, op, file, category, line, "
                        "bytes_written, file_bytes, created_category, id?} "
                        "— or for "
                        "add_batch {ok, op, file, applied:[{index, id?, "
                        "category, line}], applied_count, skipped, "
                        "skipped_count, bytes_written, file_bytes} — or "
                        "{ok:false, error, code}. A non-blocking "
                        "`advisory` string (ANTS-2125) accompanies a "
                        "successful write when the `## [Unreleased]` "
                        "section already interleaves non-heading prose "
                        "(a stray footer/separator) between its `### ` "
                        "category blocks — the entry still inserts in "
                        "canonical order, but the section layout is "
                        "malformed. "
                        "dry_run:true (ANTS-2136) previews the resolved "
                        "insert (category routing, line, `bytes`, rendered "
                        "`bullet`; add_batch echoes applied/skipped) "
                        "without writing — envelope carries dry_run:true "
                        "and `bytes` replaces `bytes_written`.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to add a CHANGELOG.md entry under "
                        "[Unreleased] instead of hand-editing it. "
                        "Mutates project state — caller_cwd required. "
                        "add_from_roadmap reuses a ROADMAP bullet's "
                        "prose by id.");

                    QJsonObject clCaller;
                    clCaller["type"] = "string";
                    clCaller["description"] = QStringLiteral(
                        "Your $PWD. REQUIRED — anchors the write to your "
                        "project's CHANGELOG.md.");

                    QJsonObject clOp;
                    clOp["type"] = "string";
                    QJsonArray clOpEnum;
                    clOpEnum.append("add");
                    clOpEnum.append("add_from_roadmap");
                    clOpEnum.append("add_batch");
                    clOpEnum.append("add_subsection");
                    clOpEnum.append("release");   // ANTS-4363
                    clOpEnum.append("normalize");
                    clOp["enum"] = clOpEnum;
                    clOp["description"] = QStringLiteral(
                        "Verb mode. Default \"add\" (summary + optional "
                        "body). \"add_from_roadmap\" reuses the cited "
                        "ROADMAP bullet's headline + Layman prose. "
                        "\"add_batch\" (ANTS-2044) writes N `entries[]` "
                        "in one read + one atomic commit — each entry "
                        "auto-detects mode (a `summary` → add; an "
                        "`id`-only entry → add_from_roadmap); per-entry "
                        "failures land in `skipped[]` while the rest "
                        "apply (parity with roadmap_log append_batch). "
                        "\"release\" (ANTS-4363) CLOSES `## [Unreleased]` "
                        "into `## [<version>] - <date>` and opens a fresh "
                        "empty `[Unreleased]` above it — the one changelog "
                        "edit every release makes, and previously the only one "
                        "this verb did not own. Takes `version` (required; "
                        "`date` defaults to today), returns the closed "
                        "section as `released_body` so a caller has its "
                        "release notes without re-reading the file, and "
                        "refuses `nothing_to_release` on an empty section or "
                        "`version_exists` when that heading already exists "
                        "(two `## [X.Y.Z]` blocks leave a notes-extraction "
                        "grep unable to choose). "
                        "\"add_subsection\" (ANTS-3584, guarded by "
                        "ANTS-4356 — it refuses `flat_section` against an "
                        "[Unreleased] carrying flat `### <category>` blocks, "
                        "because writing a dated topic there produces a MIXED "
                        "section from which op:add and op:normalize BOTH start "
                        "refusing; empty the section first to convert a "
                        "project) writes a DATED "
                        "feature-grouped block (`### <date> <Category> — "
                        "<headline>` + optional prose `body` + optional "
                        "`bullets[]`) at the TOP of [Unreleased], "
                        "newest-first — for changelogs grouped by dated "
                        "topic rather than flat categories (needs "
                        "`headline` + `category`|`kind`; `date` defaults "
                        "to today). "
                        "\"normalize\" (ANTS-3495) reorders the "
                        "`### <category>` blocks under [Unreleased] into "
                        "canonical Keep-a-Changelog order, and (ANTS-3381) "
                        "folds a stray flush-left prose line wedged between "
                        "blocks into the bullet above it as a two-space "
                        "continuation — `moves[]` reports each fold, so "
                        "preview with dry_run first (needs no "
                        "summary/body/id).");

                    QJsonObject clSummary;
                    clSummary["type"] = "string";
                    clSummary["maxLength"] = 300;
                    clSummary["description"] = QStringLiteral(
                        "Bold one-line entry summary (op:\"add\"). "
                        "Ignored under add_from_roadmap (the ROADMAP "
                        "headline is used).");

                    QJsonObject clCategory;
                    clCategory["type"] = "string";
                    QJsonArray clCatEnum;
                    clCatEnum.append("Added");
                    clCatEnum.append("Changed");
                    clCatEnum.append("Deprecated");
                    clCatEnum.append("Removed");
                    clCatEnum.append("Fixed");
                    clCatEnum.append("Security");
                    clCategory["enum"] = clCatEnum;
                    clCategory["description"] = QStringLiteral(
                        "Keep-a-Changelog category. Optional — derived "
                        "from `kind` (add) or the bullet's `Kind:` "
                        "(add_from_roadmap) when omitted.");

                    QJsonObject clKind;
                    clKind["type"] = "string";
                    clKind["description"] = QStringLiteral(
                        "Roadmap `Kind:` value used to derive `category` "
                        "when it is omitted (op:\"add\"). Same enum as "
                        "roadmap_log's kind.");

                    QJsonObject clBody;
                    clBody["type"] = "string";
                    clBody["maxLength"] = 4000;
                    clBody["description"] = QStringLiteral(
                        "Optional prose appended under the bullet as "
                        "2-space-indented continuation line(s). Under "
                        "add_from_roadmap, overrides the reused Layman "
                        "line. Pre-wrap to ~70 columns.");

                    QJsonObject clId;
                    clId["type"] = "string";
                    clId["description"] = QStringLiteral(
                        "[PROJ-NNNN] id. Appended as \"(<id>)\" on the "
                        "bullet under op:\"add\"; REQUIRED under "
                        "add_from_roadmap (the bullet to cite). "
                        "Case-sensitive.");

                    // ANTS-2044 — entries[] for op:"add_batch". Each
                    // item has the same shape as a single add /
                    // add_from_roadmap call (summary|id, category|kind,
                    // body?); mode is auto-detected per entry.
                    QJsonObject clEntryItem;
                    clEntryItem["type"] = "object";
                    QJsonObject clEntryProps;
                    clEntryProps["summary"]  = clSummary;
                    clEntryProps["category"] = clCategory;
                    clEntryProps["kind"]     = clKind;
                    clEntryProps["body"]     = clBody;
                    clEntryProps["id"]       = clId;
                    clEntryItem["properties"] = clEntryProps;
                    QJsonObject clEntries;
                    clEntries["type"]  = "array";
                    clEntries["items"] = clEntryItem;
                    clEntries["description"] = QStringLiteral(
                        "op:\"add_batch\" only — the entries to write in "
                        "one atomic commit. Each: {summary + category|"
                        "kind (+ id?, body?)} for an add, OR {id} alone "
                        "to pull from ROADMAP (add_from_roadmap). Applied "
                        "in input order; per-entry failures go to "
                        "`skipped[]`.");

                    // ANTS-3584 — op:"add_subsection" params: the dated
                    // topic headline, its date, and optional bullets rendered
                    // beneath the prose.
                    QJsonObject clHeadline;
                    clHeadline["type"] = "string";
                    clHeadline["maxLength"] = 300;
                    clHeadline["description"] = QStringLiteral(
                        "op:\"add_subsection\" — the dated topic headline; the "
                        "block heading is `### <date> <Category> — "
                        "<headline>`. Include any ids in the headline text "
                        "(e.g. \"Meadow trees cast shadows (PROJ-33)\").");

                    QJsonObject clDate;
                    clDate["type"] = "string";
                    clDate["description"] = QStringLiteral(
                        "op:\"add_subsection\" — the subsection date "
                        "(YYYY-MM-DD). Optional; defaults to today. The block "
                        "is inserted newest-first at the top of [Unreleased].");

                    QJsonObject clBulletItem;
                    clBulletItem["type"] = "object";
                    QJsonObject clBulletProps;
                    clBulletProps["summary"] = clSummary;
                    clBulletProps["body"]    = clBody;
                    clBulletProps["id"]      = clId;
                    clBulletItem["properties"] = clBulletProps;
                    QJsonObject clBullets;
                    clBullets["type"]  = "array";
                    clBullets["items"] = clBulletItem;
                    clBullets["description"] = QStringLiteral(
                        "op:\"add_subsection\" — optional bullets rendered "
                        "under the subsection prose, each `- **summary** (id)` "
                        "+ optional indented body. Each item is {summary "
                        "(required), body?, id?} (a bare string is accepted as "
                        "the summary).");

                    // ANTS-2136 — dry_run preview (parity with
                    // roadmap_log): resolve the would-be insert (id,
                    // category routing, rendered bullet, line) without
                    // writing CHANGELOG.md.
                    QJsonObject clDryRun;
                    clDryRun["type"] = "boolean";
                    clDryRun["description"] = QStringLiteral(
                        "Optional. When true, return the resolved insert "
                        "preview — `category`, `line`, `bytes`, and the "
                        "rendered `bullet` (add_batch: `applied`/`skipped` "
                        "with `bytes`) — WITHOUT writing CHANGELOG.md or "
                        "creating a category heading. A free pre-flight to "
                        "verify category routing and prose before "
                        "committing (envelope carries dry_run:true).");

                    QJsonObject clProps;
                    clProps["caller_cwd"] = clCaller;
                    clProps["op"]         = clOp;
                    {   // ANTS-4363
                        QJsonObject v; v["type"] = "string";
                                       v["description"] = QStringLiteral(
                            "op:\"release\" — the version to stamp, WITHOUT "
                            "brackets (they are added for you; a bracketed "
                            "value refuses bad_args rather than writing "
                            "`## [[1.2.3]]`, which a notes-extraction grep "
                            "would miss). The version STRING is otherwise "
                            "unconstrained, so a `-rc1` suffix or a "
                            "date-version is fine — what is fixed is the "
                            "heading shape.");
                        clProps["version"] = v;
                    }
                    clProps["summary"]    = clSummary;
                    clProps["category"]   = clCategory;
                    clProps["kind"]       = clKind;
                    clProps["body"]       = clBody;
                    clProps["id"]         = clId;
                    clProps["entries"]    = clEntries;
                    clProps["headline"]   = clHeadline;
                    clProps["date"]       = clDate;
                    clProps["bullets"]    = clBullets;
                    clProps["dry_run"]    = clDryRun;

                    QJsonObject clSchema;
                    clSchema["type"] = "object";
                    clSchema["properties"] = clProps;
                    // Only caller_cwd is unconditionally required; the
                    // per-op needs (summary for add, id for
                    // add_from_roadmap) are enforced at runtime via
                    // missing_field, mirroring roadmap_log.
                    QJsonArray clReq;
                    clReq.append(QStringLiteral("caller_cwd"));
                    clSchema["required"] = clReq;
                    clSchema["additionalProperties"] = false;
                    t["inputSchema"] = clSchema;
                    tools.append(t);
                }
                // ANTS-3533 — changelog_query: read-only structured CHANGELOG
                // reader, the symmetric read side of changelog_log.
                {
                    QJsonObject t;
                    t["name"] = "changelog_query";
                    t["description"] = QStringLiteral(
                        "Read CHANGELOG.md as structured entries {version, date, "
                        "unreleased, category, text, ids[], body?} instead of "
                        "full-reading the file — the symmetric read side of "
                        "changelog_log, mirroring roadmap_query. Filters: "
                        "version=<token|Unreleased>, category=<Added|Changed|"
                        "Deprecated|Removed|Fixed|Security>, query=<keyword>, "
                        "id / ids[] (look up entries citing a [PROJ-NNNN]). "
                        "mode: entries (default) | version_index (version "
                        "skeleton + per-category counts, no entries) | "
                        "headline_only (~10x smaller). Opt-in: include_body, "
                        "compact, fields, etag_match, offset/limit (1..500). "
                        "caller_cwd Required. Refusals: no_changelog, "
                        "format_mismatch, bad_version, bad_category, bad_mode, "
                        "bad_mode_combo, bad_case, bad_args. Etag-304 aware.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to look up CHANGELOG.md entries by version / id / "
                        "category / keyword without reading the whole file — "
                        "the read side of changelog_log (drift checks, "
                        "\"what shipped under ANTS-NNNN?\").");

                    QJsonObject cqVersion;
                    cqVersion["type"] = "string";
                    cqVersion["description"] = QStringLiteral(
                        "Filter to one version block. The bare version token "
                        "(\"0.7.100\") or \"Unreleased\" (case-insensitive). "
                        "Empty = all versions. Unknown → bad_version.");

                    // ANTS-3618 — `section` is an exact alias for `version`
                    // (a CHANGELOG's `## [X.Y.Z]` blocks ARE its sections).
                    // Declared so the dispatch layer stops flagging it in
                    // ignored_args and, more importantly, so it actually
                    // filters: callers arriving from roadmap_query reach for
                    // its vocabulary and passing `section` used to return
                    // every entry, which reads as a filter that matched
                    // everything rather than one never applied.
                    QJsonObject cqSection;
                    cqSection["type"] = "string";
                    cqSection["description"] = QStringLiteral(
                        "Alias for `version` — same values, same refusals. "
                        "Passing both with different values → bad_args.");

                    QJsonObject cqCategory;
                    cqCategory["type"] = "string";
                    QJsonArray cqCatEnum;
                    cqCatEnum.append("Added");
                    cqCatEnum.append("Changed");
                    cqCatEnum.append("Deprecated");
                    cqCatEnum.append("Removed");
                    cqCatEnum.append("Fixed");
                    cqCatEnum.append("Security");
                    cqCategory["enum"] = cqCatEnum;
                    cqCategory["description"] = QStringLiteral(
                        "Keep-a-Changelog category filter (case-sensitive). "
                        "Ignored in version_index mode. Unknown → bad_category.");

                    QJsonObject cqQuery;
                    cqQuery["type"] = "string";
                    cqQuery["description"] = QStringLiteral(
                        "Case-insensitive substring over each entry's text + "
                        "body (capped 200 chars). Composes with version/"
                        "category as a conjunction.");

                    QJsonObject cqId;
                    cqId["type"] = "string";
                    cqId["description"] = QStringLiteral(
                        "Look up entries citing a single [PROJ-NNNN] id. "
                        "Overrides version/category/query + pagination. A "
                        "genuinely-absent id → found:false (not a refusal); a "
                        "case-only mismatch → bad_case. Mutually exclusive with "
                        "ids. Cannot combine with mode:version_index.");

                    QJsonObject cqIds;
                    cqIds["type"] = "array";
                    QJsonObject cqIdsItem;
                    cqIdsItem["type"] = "string";
                    cqIds["items"] = cqIdsItem;
                    cqIds["description"] = QStringLiteral(
                        "Batch id lookup (array or comma/space string; deduped; "
                        ">100 → bad_args). A case-only mismatch is demoted to "
                        "missing_ids (no batch refusal). Mutually exclusive "
                        "with id.");

                    QJsonObject cqMode;
                    cqMode["type"] = "string";
                    QJsonArray cqModeEnum;
                    cqModeEnum.append("entries");
                    cqModeEnum.append("version_index");
                    cqModeEnum.append("headline_only");
                    cqMode["enum"] = cqModeEnum;
                    cqMode["description"] = QStringLiteral(
                        "Response mode. \"entries\" (default) returns filtered "
                        "entries; \"version_index\" returns the version "
                        "skeleton with per-category counts (no entries); "
                        "\"headline_only\" narrows each entry to {version, "
                        "category, text_oneline, ids} (~10x smaller).");

                    QJsonObject cqIncludeBody;
                    cqIncludeBody["type"] = "boolean";
                    cqIncludeBody["description"] = QStringLiteral(
                        "entries mode only: attach each entry's continuation "
                        "body. Default false. Ignored in headline_only / "
                        "version_index.");

                    QJsonObject cqOffset;
                    cqOffset["type"] = "integer";
                    cqOffset["description"] = QStringLiteral(
                        "0-based pagination start over the post-filter list.");

                    QJsonObject cqLimit;
                    cqLimit["type"] = "integer";
                    cqLimit["description"] = QStringLiteral(
                        "Page size, clamped 1..500 (mirrors roadmap_query). "
                        "Omitted → auto soft-cap; truncation emits truncated + "
                        "next_offset. ANTS-3576 — in entries mode (not "
                        "headline_only / include_body), a would-be-truncated "
                        "page auto-downshifts to the lean headline_only entry "
                        "shape {version, category, ids, text_oneline} and "
                        "re-pages so the tail is kept; the response then carries "
                        "downshifted:true and its rows are headline-shaped.");

                    QJsonObject cqEncoding;
                    cqEncoding["type"] = "string";
                    QJsonArray cqEncEnum;
                    cqEncEnum.append("json");
                    cqEncEnum.append("tabular");
                    cqEncoding["enum"] = cqEncEnum;
                    cqEncoding["description"] = QStringLiteral(
                        "Optional (ANTS-2090). \"tabular\" packs the entries/"
                        "versions array columnar (30-60% smaller). Default "
                        "\"json\".");

                    QJsonObject cqProps;
                    cqProps["caller_cwd"]   = makeCallerCwdReadProp();
                    cqProps["version"]      = cqVersion;
                    cqProps["section"]      = cqSection;        // ANTS-3618
                    cqProps["category"]     = cqCategory;
                    cqProps["query"]        = cqQuery;
                    cqProps["id"]           = cqId;
                    cqProps["ids"]          = cqIds;
                    cqProps["mode"]         = cqMode;
                    cqProps["include_body"] = cqIncludeBody;
                    cqProps["offset"]       = cqOffset;
                    cqProps["limit"]        = cqLimit;
                    cqProps["etag_match"]   = makeEtagMatchProp();  // ANTS-1499
                    cqProps["fields"]       = makeFieldsProp();     // ANTS-1720
                    cqProps["compact"]      = makeCompactProp();    // ANTS-2091
                    cqProps["encoding"]     = cqEncoding;           // ANTS-2090

                    QJsonObject cqSchema;
                    cqSchema["type"] = "object";
                    cqSchema["properties"] = cqProps;
                    QJsonArray cqReq;
                    cqReq.append(QStringLiteral("caller_cwd"));
                    cqSchema["required"] = cqReq;
                    cqSchema["additionalProperties"] = false;
                    t["inputSchema"] = cqSchema;
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
                        "code=tools_not_ready. ANTS-1985: pass "
                        "catalog:true (no name) to get the full verb "
                        "catalog — every registered tool grouped by "
                        "category with its one-line selection_hint, in "
                        "one call. See docs/specs/ANTS-1399.md + "
                        "ANTS-1985.md.");
                    t["selection_hint"] = QStringLiteral(
                        "Use to fetch one tool's descriptor without "
                        "re-paying for the full tools/list snapshot "
                        "(~80 B vs ~5 KiB), or catalog:true for the "
                        "whole toolkit grouped by category (ANTS-1985). "
                        "Surfaces selection_hint field (ANTS-1453).");
                    QJsonObject schema;
                    schema["type"] = "object";
                    QJsonObject nameProp;
                    nameProp["type"] = "string";
                    nameProp["description"] = QStringLiteral(
                        "Registered tool name (e.g. "
                        "\"verify_changes\", \"file_outline\"). Omit "
                        "when catalog:true.");
                    // ANTS-1985 — catalog mode: return every verb
                    // grouped by category instead of one descriptor.
                    QJsonObject catalogProp;
                    catalogProp["type"] = "boolean";
                    catalogProp["description"] = QStringLiteral(
                        "When true, ignore `name` and return the full "
                        "verb catalog: {ok, catalog:{<category>:"
                        "[{name, selection_hint}]}, tool_count, "
                        "category_count}. One call replaces ~70 "
                        "per-tool probes (ANTS-1985).");
                    QJsonObject props;
                    props["name"] = nameProp;
                    props["catalog"] = catalogProp;
                    schema["properties"] = props;
                    // ANTS-1985 — neither arg is unconditionally
                    // required: name-mode needs `name`, catalog-mode
                    // needs `catalog:true`. The handler enforces the
                    // choice at runtime, so `required` is empty (was
                    // `["name"]` pre-1985).
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
                    props["fields"]       = makeFieldsProp();      // ANTS-1720
                    props["compact"]      = makeCompactProp();     // ANTS-2091
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
                        // Repo / docs.
                        {QStringLiteral("roadmap_query"),     {1700, 12000}},
                        {QStringLiteral("roadmap_log"),       {200,  600}},
                        // ANTS-3855 — roadmap_migrate: counts + a bounded
                        // notes[] (200 entries max), no roadmap text echoed
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
                        {QStringLiteral("session_brief"),     {300,  1200}},
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
                        // ANTS-1279 — one call replaces partition + N briefs.
                        {QStringLiteral("indie_review_orchestrate"),  {4000, 20000}},
                        // Test-audit.
                        {QStringLiteral("test_audit_partition"),       {1500, 6000}},
                        {QStringLiteral("test_audit_brief"),           {2000, 8000}},
                        {QStringLiteral("test_audit_synthesis_prompt"),{2500, 10000}},
                        {QStringLiteral("test_audit_fold_in"),         {1000, 4000}},
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
                    // ANTS-1548 — changelog_log (writer) + ANTS-3533
                    // changelog_query (reader) are the CHANGELOG siblings
                    // of the roadmap family.
                    if (name.startsWith(QStringLiteral("changelog_")))
                        return QStringLiteral("roadmap");
                    // ANTS-1414 — lane-source-agnostic alias bucket.
                    if (name == QLatin1String("cross_doc_diff"))
                        return QStringLiteral("cold-eyes");
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
                        name == QLatin1String("doc_symbols") ||
                        name == QLatin1String("spec_lint") ||   // ANTS-3662
                        // ANTS-4108 — spec_conformance: spec_lint's executable
                        // sibling; same project-scoped document reader family.
                        name == QLatin1String("spec_conformance") ||
                        name == QLatin1String("doc_dedup") ||   // ANTS-3660
                        // ANTS-2161 — project_settings: project-scoped
                        // layout-config detect + create/update.
                        name == QLatin1String("project_settings") ||
                        // ANTS-2093 — project_query: project-scoped read
                        // (runs a sandboxed snippet over caller_cwd's files).
                        name == QLatin1String("project_query") ||
                        name == QLatin1String("project_layout") ||
                        name == QLatin1String("subsystem") ||
                        // ANTS-1569 — current_state is a project-scoped
                        // read aggregator (joins project_layout's family).
                        name == QLatin1String("current_state") ||
                        // ANTS-1724 — session_brief is the compact variant.
                        name == QLatin1String("session_brief") ||
                        // ANTS-1883 — session_orient bundles
                        // current_state + project_layout + roadmap
                        // section_index; same workspace family.
                        name == QLatin1String("session_orient"))
                        return QStringLiteral("workspace");
                    if (name == QLatin1String("session_memory") ||
                            name == QLatin1String("workflow_state"))
                        return QStringLiteral("mcp-state");
                    // ANTS-1735 — model_switch_stats: autonomous
                    // model-switcher effectiveness scorecard.
                    if (name == QLatin1String("model_switch_stats"))
                        return QStringLiteral("model");
                    if (name == QLatin1String("plan_template"))
                        return QStringLiteral("plan");
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
                        name == QLatin1String("find_caller"))
                        return QStringLiteral("symbol");
                    // ANTS-1305 — shape matcher.
                    if (name == QLatin1String("similar_code"))
                        return QStringLiteral("pattern");
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
                        // ANTS-1581(b) stamped every cold_eyes_ /
                        // indie_review_ verb with "the skill orchestrates
                        // this itself and does not call this tool", so a
                        // session reading the deferred-tool list would not
                        // mistake naming parity for "this is the canonical
                        // path"; ANTS-3639 then had to exempt the verbs the
                        // skills DO mandate.
                        //
                        // REVERSED 2026-07-28 (global CLAUDE.md §18): the
                        // MCP verbs are now the default path and the raw
                        // tools are the fallback, so "the skill does not
                        // call this" is false of both families — and a note
                        // steering the reader away from the verb "unless
                        // building your own pipeline" argues against the
                        // standing rule.
                        //
                        // One carve-out survives, and it is about what the
                        // verb DOES rather than who calls it:
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
                    // ANTS-2158 — exempt the highest-frequency verbs from
                    // Claude Code's MCP tool-search DEFERRAL so they are
                    // callable without a ToolSearch round-trip (the
                    // "deferred-schema tax" that nudged sessions back to raw
                    // grep/Read/Edit). Honoured by Claude Code v2.1.121+ via
                    // the tool's `_meta`; older clients ignore the field
                    // (graceful). The set is small on purpose — each
                    // always-loaded tool costs context, so only the verbs
                    // that most directly replace always-loaded built-ins
                    // (Bash grep / Read / Edit, ROADMAP/CHANGELOG edits).
                    // See docs/standards/mcp-behavioural-notes.md.
                    static const QSet<QString> kEagerVerbs = {
                        QStringLiteral("workspace_search"),
                        QStringLiteral("find_definition"),
                        QStringLiteral("file_outline"),
                        QStringLiteral("read_region"),
                        QStringLiteral("roadmap_log"),
                        QStringLiteral("changelog_log"),
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
                    m_toolProviders.find(toolName) != m_toolProviders.end();
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
                // and, for two of them (get_text / recent_errors), an
                // explicit `tab` index. With NO usable routing key they
                // fall back to the focused tab — the cross-tenant leak
                // ANTS-1404 closed for Required tools, deferred from
                // Phase 3a because the routing-vs-anchoring semantics
                // needed their own spec pass. Refuse before the
                // rate-limit + cache, mirroring the Required branch.
                // A stray `tab` on a cwd-only tool is NOT a routing key
                // (the handler ignores it), else it would bypass the
                // gate and still fall back to focused. See
                // docs/standards/mcp-error-codes.md § 3 + docs/specs/ANTS-1415.md.
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
                        responseText = it->second.handler(argsObj);
                        toolHandled = true;
                    }
                }
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
                // steady-state cost is one cheap key-set diff. Refusal
                // envelopes (ok:false) are left untouched — the caller already
                // reads their error. Empty m_toolParamKeys (no tools/list yet)
                // degrades to no advisory.
                if (toolHandled && !cachedHit &&
                    m_toolParamKeys.contains(toolName)) {
                    const QStringList ignored = mcp::ignoredArgs(
                        argsObj, m_toolParamKeys.value(toolName));
                    if (!ignored.isEmpty()) {
                        QJsonParseError perr{};
                        const QJsonDocument advDoc = QJsonDocument::fromJson(
                            responseText.toUtf8(), &perr);
                        if (perr.error == QJsonParseError::NoError &&
                            advDoc.isObject()) {
                            QJsonObject env = advDoc.object();
                            if (env.value(QStringLiteral("ok"))
                                    != QJsonValue(false)) {
                                env[QStringLiteral("ignored_args")] =
                                    QJsonArray::fromStringList(ignored);
                                responseText = QString::fromUtf8(
                                    QJsonDocument(env)
                                        .toJson(QJsonDocument::Compact));
                            }
                        }
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
                bool etagUnchanged = false;
                if (toolHandled && isEtagSupportedTool(toolName)) {
                    responseText = applyEtagPattern(
                        toolName, argsObj, responseText, &etagUnchanged);
                    if (etagUnchanged) {
                        dispatchResult = QStringLiteral("etag_unchanged");
                    }
                }
                // ANTS-1720 — `fields=` projection. Runs after the etag
                // short-circuit (so the etag is computed on the
                // unfiltered canonical body) and before the wrap (so the
                // hash covers the JSON envelope, not the wrapper). Skipped
                // on the etag short-circuit — {ok,unchanged,etag} has no
                // content to narrow. To retain the etag through a narrowed
                // call, the caller lists "etag" in `fields`.
                if (toolHandled && !etagUnchanged &&
                    mcp::isFieldProjectionTool(toolName)) {
                    const QJsonValue fv =
                        argsObj.value(QStringLiteral("fields"));
                    if (fv.isArray()) {
                        responseText = mcp::projectFields(
                            responseText, fv.toArray());
                    }
                }
                // ANTS-2091 — opt-in `compact:true` drops dead-weight
                // fields (null / false / "" / [] / {}) the model never
                // reads. After fields= (compacts the narrowed body too)
                // and before the etag-unchanged short-circuit is irrelevant
                // — skipped on a 304, which is already minimal. Protected
                // keys (ok/code/error/etag/found/unchanged) survive at
                // every level (see mcp::compactEnvelope).
                // ANTS-2085 — resolve `compact` from the per-call arg when
                // present (true OR false both win), else fall back to the
                // session/user terse default (mcp::terseDefault(), driven by
                // the claude.mcp_terse_responses config key — on by default so
                // token-saving needs no per-call flag). Absent ⟺ default makes
                // the fallback lossless; a caller needing empty-vs-absent
                // passes compact:false.
                if (toolHandled && !etagUnchanged &&
                    mcp::isFieldProjectionTool(toolName)) {
                    const QJsonValue compactArg =
                        argsObj.value(QStringLiteral("compact"));
                    const bool wantCompact = compactArg.isBool()
                        ? compactArg.toBool()
                        : mcp::terseDefault();
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
                // 304s, and arrays that don't shrink. See docs/specs/ANTS-2090.md.
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
                // unchanged on any write failure. See docs/specs/ANTS-2094.md.
                if (toolHandled && !etagUnchanged && !rawRequested &&
                    mcp::isOffloadEligible(toolName) &&
                    mcp::offloadRequested(argsObj)) {
                    // ANTS-3552 — the >=threshold / >head boundary is the
                    // extracted mcp::shouldOffload predicate (behaviourally
                    // tested; offloadBody has no internal threshold guard).
                    const qint64 bodyBytes = responseText.toUtf8().size();
                    if (mcp::shouldOffload(bodyBytes)) {
                        responseText = mcp::offloadBody(toolName, responseText);
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
                    // ANTS-2218 — raw verbatim frame (opt-in, read verbs) vs
                    // the default lossy tag-scrub. Control-plane bypasses both.
                    const QString wrapped =
                        isControlPlane ? responseText
                        : rawRequested ? wrapMcpDataRaw(toolName, responseText)
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
                    recordDispatch(toolName, argsObj, argBytes,
                                   /*rawBytes=*/buf.size(), outBytes,
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
                                   /*rawBytes=*/buf.size(),
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

            // ANTS-2101 — the dispatch above may have run a nested event
            // loop (audit_run pumps QProcesses), during which the peer's
            // disconnect freed this socket via disconnected -> deleteLater.
            // Bail before touching a dangling pointer (covers both the
            // notification-disconnect and the response write below).
            if (!guard || socket->state() != QLocalSocket::ConnectedState)
                return;

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
            // ANTS-1769 — append a '\n' end-of-reply terminator. Compact
            // JSON contains no raw newline bytes (string newlines are
            // escaped as the two chars \n), so a trailing raw '\n' is an
            // unambiguous "reply complete" marker. The client (mcp-bridge)
            // treats a missing terminator on a parse failure as truncation
            // (-32000) rather than malformed (-32700). Back-compatible: an
            // older client that reads to EOF + json.loads ignores the
            // trailing whitespace.
            resp.append('\n');
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
    // ANTS-1670 M2 — symmetric open-tag scrub. The close-tag breakout
    // above is only half the wrapper: a payload carrying a literal
    // `<ants_mcp_data …>` open tag (or a case/whitespace variant) could
    // spoof a nested wrapper-open for a consuming assistant that matches
    // the open tag tolerantly, desyncing the real frame. Neutralise the
    // open form too. (The `\b` after the tag name leaves the
    // `<ants_mcp_data_escaped/>` sentinel emitted just above untouched:
    // `data` is followed by `_`, itself a word char, so no word boundary
    // exists there and the sentinel is never re-matched.)
    static const QRegularExpression openTagVariantRe(
        QStringLiteral(R"(<\s*ants_mcp_data\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    sanitised.replace(openTagVariantRe,
                      QStringLiteral("<ants_mcp_data_escaped/>"));
    // ANTS-1996 — neutralise XML/HTML comment markers. The close-tag
    // scrub above is defeated by a comment desync: an unterminated
    // `<!--` in the payload swallows the real `</ants_mcp_data>` from a
    // consuming assistant that treats markup comments structurally, so
    // the literal close tag is never seen and the wrap silently runs on
    // into following content; a stray `-->` can re-open surrounding
    // context the same way. Replace both markers with a visibly-modified
    // but inert form (a space breaks the token) so a hostile payload
    // can't smuggle structural ambiguity past the close-tag scrub.
    sanitised.replace(QStringLiteral("<!--"), QStringLiteral("<!- -"));
    sanitised.replace(QStringLiteral("-->"),  QStringLiteral("- ->"));
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

// --- ANTS-2218 — opt-in raw (verbatim) framing for content reads ---
//
// wrapMcpData (above) neutralises any literal </ants_mcp_data> (plus open-tag
// and `<!--`/`-->` variants) in the payload. That scrub is DELIBERATELY lossy
// and irreversible: a hostile file/scrollback must not be able to forge the
// frame-close and have following bytes read as trusted prose, and any escaping
// a good agent could invert a hostile normalising tokeniser could invert too —
// reopening the ANTS-1294/1670/1996 breakout. The cost is that an agent reading
// frame-sensitive SOURCE (e.g. this file, a spec, HTML/markdown with comments)
// sees doctored bytes and would corrupt the file if it built an Edit from them
// (ANTS-2218).
//
// Raw mode is the opt-in escape hatch: emit the payload VERBATIM so the agent
// gets true bytes, made safe by an UNFORGEABLE delimiter instead of a scrub.
// The frame tag carries a per-call nonce — a truncated SHA-256 of the payload,
// verified absent from the payload — so </ants_mcp_data_raw__<nonce>> cannot
// occur in the content and there is nothing reversible for a hostile consumer
// to recover. Falls back to the safe scrub in the (infeasible) case that no
// collision-free nonce is found. Honoured only for mcp::isRawEligible read
// verbs at the dispatch site. See tests/features/mcp_raw_read.
QString ClaudeIntegration::wrapMcpDataRaw(const QString &toolName,
                                          const QString &payload) {
    // Derive the nonce from the payload's own hash, then widen until it is
    // absent from the payload. A payload that contains its own SHA-256 is a
    // preimage problem (infeasible), but verify-and-widen keeps unforgeability
    // a proof rather than an assumption.
    const QByteArray digest =
        QCryptographicHash::hash(payload.toUtf8(), QCryptographicHash::Sha256)
            .toHex();
    QString nonce;
    for (int len = 16; len <= digest.size(); len += 8) {
        nonce = QString::fromLatin1(digest.left(len));
        if (!payload.contains(nonce)) break;
    }
    if (payload.contains(nonce)) {
        // Unreachable in practice. Rather than emit a forgeable frame, fall
        // back to the safe (lossy) scrub.
        return wrapMcpData(toolName, payload);
    }
    // Tool-name attribute hardening (mirrors wrapMcpData).
    QString safeTool = toolName;
    safeTool.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    safeTool.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    safeTool.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    safeTool.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return QStringLiteral(
               "<ants_mcp_data_raw__%1 tool=\"%2\">%3</ants_mcp_data_raw__%1>")
        .arg(nonce, safeTool, payload);
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

// ANTS-3396 — async audit_run job registry. Bounded/reaped/evicted so a
// runaway or process-lifetime accumulation can never grow unbounded.
QString ClaudeIntegration::auditJobRegister(
        const QString &root, qint64 startedMs) {
    QMutexLocker lk(&m_auditJobsMutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // (1) Reap entries older than the reap window (same as the in-flight
    // gate). A stale running entry ages out too — its result stays durable
    // on disk and the poll then reads `expired`.
    for (auto it = m_auditJobs.begin(); it != m_auditJobs.end(); ) {
        if (now - it.value().startedMs > kAuditJobReapMs)
            it = m_auditJobs.erase(it);
        else
            ++it;
    }
    // (2) Still at the cap → evict the oldest TERMINAL entry (by startedMs
    // — the only timestamp stored; a deliberate simplification, harmless on
    // the 16-entry single-project cap). A running entry is never evicted.
    if (m_auditJobs.size() >= kAuditJobsMax) {
        QString victim;
        qint64  oldest = 0;
        bool    have = false;
        for (auto it = m_auditJobs.constBegin();
             it != m_auditJobs.constEnd(); ++it) {
            const AuditJob &j = it.value();
            if (j.status != QLatin1String("running")
                && (!have || j.startedMs < oldest)) {
                oldest = j.startedMs;
                victim = it.key();
                have   = true;
            }
        }
        // (3) All entries running → no room; caller emits `too_many_jobs`.
        if (!have) return QString();
        m_auditJobs.remove(victim);
    }
    const QString jobId =
        QStringLiteral("audit-") + QString::number(m_auditJobNextId++);
    AuditJob j;
    j.status    = QStringLiteral("running");
    j.root      = root;
    j.startedMs = startedMs;
    m_auditJobs.insert(jobId, j);
    return jobId;
}

void ClaudeIntegration::auditJobComplete(
        const QString &jobId, const AuditJob &result) {
    QMutexLocker lk(&m_auditJobsMutex);
    auto it = m_auditJobs.find(jobId);
    if (it == m_auditJobs.end())
        return;  // reaped/evicted — the result is durable on disk.
    // Overlay the terminal fields; keep the original start time + root.
    AuditJob j     = result;
    j.startedMs    = it.value().startedMs;
    j.root         = it.value().root;
    it.value()     = j;
}

QJsonObject ClaudeIntegration::auditJobPollEnvelope(
        const QString &jobId, const QString &callerRoot) const {
    QMutexLocker lk(&m_auditJobsMutex);
    QJsonObject env;
    env["ok"]     = true;
    env["job_id"] = jobId;
    auto it = m_auditJobs.constFind(jobId);
    // A miss OR a job owned by a different project root both read as
    // `expired` — the caller can only see its own project's jobs, and a
    // cross-root probe is indistinguishable from a genuine miss.
    if (it == m_auditJobs.constEnd() || it.value().root != callerRoot) {
        // Registry miss — normal terminal poll status, NOT a refusal.
        env["status"] = QStringLiteral("expired");
        env["hint"]   = QStringLiteral(
            "job not in registry (completed long ago or evicted); read the "
            "latest result via last_audit_summary.");
        return env;
    }
    const AuditJob &j = it.value();
    env["status"] = j.status;
    if (j.status == QLatin1String("running")) {
        env["started_at_ms"] = j.startedMs;
        env["elapsed_ms"]    =
            QDateTime::currentMSecsSinceEpoch() - j.startedMs;
    } else if (j.status == QLatin1String("error")) {
        // Branch on `status`, not `ok` — the poll itself succeeded.
        // `code` is job-scoped; omit when runAudit left it empty (error
        // carries the detail). NOT a poll-level refusal code.
        if (!j.code.isEmpty()) env["code"] = j.code;
        env["error"] = j.error;
    } else {  // "done"
        if (!j.cachePath.isEmpty()) env["cache_path"] = j.cachePath;
        env["total_raw"]        = j.totalRaw;
        env["total_actionable"] = j.totalActionable;
        env["partial"]          = j.partial;
        QJsonArray inc;
        for (const QString &t : j.incompleteTools) inc.append(t);
        env["incomplete_tools"] = inc;
        // ANTS-3585 — same richer surfaces as the sync provider (mainwindow):
        // per-tool truncated/crashed detail + the zero-coverage parse-failure
        // list. Omitted when empty.
        if (!j.incompleteToolsDetail.isEmpty())
            env["incomplete_tools_detail"] = j.incompleteToolsDetail;
        if (!j.parseFailures.isEmpty()) {
            QJsonArray pf;
            for (const QString &f : j.parseFailures) pf.append(f);
            env["parse_failures"] = pf;
            // ANTS-3706 — same detail sibling as the sync provider.
            if (!j.parseFailuresDetail.isEmpty())
                env["parse_failures_detail"] = j.parseFailuresDetail;
        }
        if (j.noChanges) env["no_changes"] = true;
        env["read_full_with"]   = QStringLiteral("last_audit_summary");
    }
    return env;
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
    // ANTS-3396 — audit_poll reads the async-audit job registry.
    // Required for parity with audit_run (the dispatcher refuses
    // caller_cwd_required before the read-only poll runs).
    if (toolName == QStringLiteral("audit_poll"))         return C::Required;
    // ANTS-1430 — project_layout reads from the tenant-hashed
    // session_memory store. Joins session_memory in the gated
    // Required set (see ANTS-1336 INV-7 amendment).
    if (toolName == QStringLiteral("project_layout"))     return C::Required;
    // ANTS-1855 — read_log resolves project-relative paths + anchors
    // tenancy; Required even for the debug-log default so the tool can't
    // be used as an unscoped file reader.
    if (toolName == QStringLiteral("read_log"))            return C::Required;
    // ANTS-2021 — read_region resolves a project-relative path + anchors
    // tenancy; Required.
    if (toolName == QStringLiteral("read_region"))         return C::Required;
    // ANTS-2219 — read_regions: batched sibling; each item resolves under
    // the same root, so caller_cwd is Required.
    if (toolName == QStringLiteral("read_regions"))        return C::Required;
    // ANTS-2094 — read_spill resolves a GLOBAL content-addressed handle
    // under ~/.cache (not project-scoped), so caller_cwd is neither an
    // anchor nor required; Optional accepts the absent case.
    if (toolName == QStringLiteral("read_spill"))          return C::Optional;
    // ANTS-2022 — apply_edits writes project files anchored by caller_cwd +
    // PathValidation (roadmap_log/changelog_log posture, no RcGate); Required.
    if (toolName == QStringLiteral("apply_edits"))         return C::Required;
    // ANTS-1637 — codebase_index is a project-scoped structural-map reader
    // keyed on the resolved root; Required.
    if (toolName == QStringLiteral("codebase_index"))      return C::Required;
    // ANTS-2139 — docs_index is a project-scoped documentation-map reader
    // keyed on the resolved root; Required (sibling of codebase_index).
    if (toolName == QStringLiteral("docs_index"))          return C::Required;
    // ANTS-3601 — doc_integrity is a project-scoped doc-consistency reader
    // keyed on the resolved root; Required.
    if (toolName == QStringLiteral("doc_integrity"))       return C::Required;
    // ANTS-3636 — doc_citations resolves citations against the focused
    // project's tree; Required.
    if (toolName == QStringLiteral("doc_citations"))       return C::Required;
    // ANTS-3661 — doc_symbols walks the focused project's docs AND resolves
    // against its source tree; both need the caller's anchor.
    if (toolName == QStringLiteral("doc_symbols"))         return C::Required;
    // ANTS-3662 — spec_lint walks the focused project's specs_dir and reads its
    // format standard from the same root; Required.
    if (toolName == QStringLiteral("spec_lint"))           return C::Required;
    // ANTS-4108 — spec_conformance resolves a required `path` under the focused
    // project's root and reads only that document; Required.
    if (toolName == QStringLiteral("spec_conformance"))    return C::Required;
    // ANTS-3660 — doc_dedup walks the focused project's docs_dir; Required.
    if (toolName == QStringLiteral("doc_dedup"))           return C::Required;
    // ANTS-2161 — project_settings reads/writes <root>/.ants/project.json
    // anchored on the resolved root; Required.
    if (toolName == QStringLiteral("project_settings"))    return C::Required;
    // ANTS-2093 — project_query: caller_cwd is both the project anchor and
    // the FS-confinement root for the snippet's project.* reads.
    if (toolName == QStringLiteral("project_query"))       return C::Required;
    // ANTS-1961 / ANTS-1962 — feedback verbs resolve a relative `path`
    // off caller_cwd and anchor tenancy; Required even though the
    // canonical case is an absolute shared-root path.
    if (toolName == QStringLiteral("feedback_query"))      return C::Required;
    if (toolName == QStringLiteral("feedback_log"))        return C::Required;
    // ANTS-2129 — audit_falsepos_log resolves <root>/.ants_review_falsepos.jsonl
    // off caller_cwd; Required (matches the other root-resolving write verbs).
    if (toolName == QStringLiteral("audit_falsepos_log")) return C::Required;
    // ANTS-1713 — audit_dismiss resolves <root>/.audit_cache/learned-fp.jsonl
    // off caller_cwd; Required (same shape as audit_falsepos_log).
    if (toolName == QStringLiteral("audit_dismiss"))      return C::Required;
    // ANTS-1963 — spec_log resolves docs/specs|phases/<id>.md under the
    // caller's project root; Required matches spec_query.
    if (toolName == QStringLiteral("spec_log"))            return C::Required;
    // ANTS-1435 — session_memory: dispatcher refuses empty
    // caller_cwd upstream (Required). The handler still has a
    // body-level cwd_missing for the IPC path which bypasses the
    // contract — kept for diagnostic parity. Asymmetric internal
    // routing: reads anchor to caller_cwd, writes match focused tab.
    if (toolName == QStringLiteral("session_memory"))     return C::Required;
    if (toolName == QStringLiteral("session_brief"))      return C::Required;
    if (toolName == QStringLiteral("session_orient"))     return C::Required;  // ANTS-1883
    if (toolName == QStringLiteral("workflow_state"))     return C::Required;
    // TabSpecific — classified but not enforced in Phase 3a. The
    // ANTS-1392 routing semantics (caller_cwd as a tab-routing key)
    // need their own spec pass before refusal makes sense.
    if (toolName == QStringLiteral("get_scrollback"))     return C::TabSpecific;
    if (toolName == QStringLiteral("get_text"))           return C::TabSpecific;
    // ANTS-1301 — reads the focused terminal's scrollback.
    if (toolName == QStringLiteral("recent_errors"))      return C::TabSpecific;
    // ANTS-1312 — reads the focused terminal's current selection.
    if (toolName == QStringLiteral("last_selection"))    return C::TabSpecific;
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
    // ANTS-3855 — roadmap_migrate loads ONE project into the roadmap store,
    // resolved from caller_cwd. A migration is a whole-project operation and
    // has nothing to fall back to when no project is named.
    if (toolName == QStringLiteral("roadmap_migrate"))    return C::Required;
    // ANTS-1548 — changelog_log mutates CHANGELOG.md under the caller's
    // project root. Required for the same reason as roadmap_log.
    if (toolName == QStringLiteral("changelog_log"))      return C::Required;
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
    if (toolName == QStringLiteral("changelog_query"))    return C::Required;  // ANTS-3533
    if (toolName == QStringLiteral("subsystem"))          return C::Required;
    if (toolName == QStringLiteral("workspace_search"))   return C::Required;
    // ANTS-1636 — find_sources: project-scoped topic-to-files scan.
    if (toolName == QStringLiteral("find_sources"))       return C::Required;
    // ANTS-3368 — co_change_family: project-scoped repo-wide field scan.
    if (toolName == QStringLiteral("co_change_family"))   return C::Required;
    if (toolName == QStringLiteral("file_outline"))       return C::Required;
    if (toolName == QStringLiteral("mutation_probe"))     return C::Required;  // ANTS-4398
    if (toolName == QStringLiteral("plan_template"))      return C::Required;
    // ANTS-1569 — current_state aggregator: project-scoped read over
    // ROADMAP + git + audit cache. The ANTS-1520 fall-through default
    // already returns Required; this explicit branch is declarative
    // parity with sibling project-scoped tools.
    if (toolName == QStringLiteral("current_state"))      return C::Required;
    // ANTS-1735 — model_switch_stats: read-only ledger aggregation scoped to the
    // caller's project. Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("model_switch_stats"))  return C::Required;
    // ANTS-1309 + ANTS-1308 — spec-aware MCP tools. Read under
    // docs/specs/ which lives strictly inside the project root, so
    // Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("spec_query"))         return C::Required;
    if (toolName == QStringLiteral("invariant_check"))    return C::Required;
    // ANTS-1299 + ANTS-1300 — build/test cache MCP tools. Both ops
    // (record + read) scope to <root>/.audit_cache/; Required matches
    // sibling project-scoped tools.
    if (toolName == QStringLiteral("build_status"))       return C::Required;
    if (toolName == QStringLiteral("test_results"))       return C::Required;
    // ANTS-1302 — focused_test reads/runs under the caller's project root.
    if (toolName == QStringLiteral("focused_test"))       return C::Required;
    // ANTS-3745 — build_target_for parses the caller's own CMakeLists.txt.
    if (toolName == QStringLiteral("build_target_for"))   return C::Required;
    // ANTS-1303 — symbol queries scan the project tree under the
    // caller's root; Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("find_definition"))    return C::Required;
    if (toolName == QStringLiteral("find_caller"))        return C::Required;
    // ANTS-1305 — similar_code scans the project tree under the
    // caller's root; Required matches sibling project-scoped readers.
    if (toolName == QStringLiteral("similar_code"))        return C::Required;
    // ANTS-3716 — cited_by searches the caller's doc tree; Required, same as
    // workspace_search, whose rg runner it shares.
    if (toolName == QStringLiteral("cited_by"))            return C::Required;
    // ANTS-1306 + ANTS-1307 — task-start context composers read under
    // the caller's project root; Required matches sibling readers.
    if (toolName == QStringLiteral("task_priors"))        return C::Required;
    if (toolName == QStringLiteral("project_conventions")) return C::Required;
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
    if (toolName == QStringLiteral("indie_review_orchestrate"))      return C::Required;  // ANTS-1279
    if (toolName == QStringLiteral("indie_review_partition"))        return C::Required;
    if (toolName == QStringLiteral("indie_review_synthesis_prompt")) return C::Required;
    // Test-audit verb cluster (ANTS-1397).
    if (toolName == QStringLiteral("test_audit_brief"))            return C::Required;
    if (toolName == QStringLiteral("test_audit_fold_in"))          return C::Required;
    if (toolName == QStringLiteral("test_audit_partition"))        return C::Required;
    if (toolName == QStringLiteral("test_audit_synthesis_prompt")) return C::Required;
    if (toolName == QStringLiteral("test_audit_recheck"))          return C::Required;

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
        || toolName == QStringLiteral("changelog_query")   // ANTS-3533
        || toolName == QStringLiteral("file_outline")
        // ANTS-2021 — read_region: a re-read of an unchanged slice 304s,
        // the core "free re-read" win.
        || toolName == QStringLiteral("read_region")
        // ANTS-1637 — codebase_index: an unchanged warm query 304s.
        || toolName == QStringLiteral("codebase_index")
        // ANTS-2139 — docs_index: an unchanged warm query 304s.
        || toolName == QStringLiteral("docs_index")
        // ANTS-3601 — doc_integrity: unchanged docs → identical findings → 304.
        || toolName == QStringLiteral("doc_integrity")
        // ANTS-3636 — doc_citations: unchanged doc + unchanged targets → an
        // identical answer → 304.
        || toolName == QStringLiteral("doc_citations")
        // ANTS-3716 — cited_by: a pure function of (tree, request), and its
        // arrays all carry a stated order precisely so the hash is stable.
        || toolName == QStringLiteral("cited_by")
        // ANTS-3368 — co_change_family: a pure function of (tree, stems), and
        // INV-6 pins a total order on files and sites, so the hash is stable.
        || toolName == QStringLiteral("co_change_family")
        || toolName == QStringLiteral("doc_symbols")  // ANTS-3661
        || toolName == QStringLiteral("spec_lint")    // ANTS-3662
        || toolName == QStringLiteral("doc_dedup")    // ANTS-3660
        || toolName == QStringLiteral("last_audit_summary")
        || toolName == QStringLiteral("get_environment")
        || toolName == QStringLiteral("tab_list")
        || toolName == QStringLiteral("subsystem")
        || toolName == QStringLiteral("git_state")
        // ANTS-1583 — roadmap_branch_drift response is small but the
        // drift list rarely changes between calls (ROADMAP mtime +
        // HEAD reachability snapshot), so the etag 304 round-trip
        // saves the full re-emit on stable repos.
        || toolName == QStringLiteral("roadmap_branch_drift")
        // ANTS-1569 — current_state aggregator. Three upstream
        // payloads (roadmap_query + git_state + last_audit_summary)
        // composed into one envelope; the etag covers the union, so
        // a session re-asking "what's the state" between upstream
        // changes short-circuits.
        || toolName == QStringLiteral("current_state")
        // ANTS-1735 — model_switch_stats: the aggregate is stable between
        // switches, so a session re-polling its scorecard short-circuits.
        || toolName == QStringLiteral("model_switch_stats")
        // ANTS-1724 — session_brief: compact current_state variant.
        || toolName == QStringLiteral("session_brief")
        // ANTS-1883 — session_orient: composer over three ETag-
        // eligible verbs, naturally ETag-eligible too.
        || toolName == QStringLiteral("session_orient")
        // ANTS-1299 + ANTS-1300 — build/test caches. The envelope on
        // op=read is stable between record calls, so re-reads from
        // peer sessions / tabs short-circuit. Etag injection is
        // op-agnostic at the dispatcher (record responses also carry
        // an etag); the field is only semantically useful on op=read.
        || toolName == QStringLiteral("build_status")
        || toolName == QStringLiteral("test_results")
        // ANTS-1961 — feedback_query: the delta is stable between
        // contributor appends, so a session re-querying the same file in
        // one review short-circuits. The etag is the sha256 of the
        // envelope (which carries the file-derived delta), so it changes
        // iff the file content changes. (feedback_log / spec_log are
        // writers — deliberately NOT etag-enabled.)
        || toolName == QStringLiteral("feedback_query");
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
// constants" (kRateLimitCheapCap / kRateLimitBriefAssemblyCap /
// kRateLimitExpensiveCap). The `*ForTest` setters write here;
// production callers never touch it.
int g_rateLimitCheapCapOverride         = -1;
int g_rateLimitBriefAssemblyCapOverride = -1;
int g_rateLimitExpensiveCapOverride     = -1;
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
    // ANTS-1629 + ANTS-1643 — BriefAssembly tier (30/min). Brief-
    // generation verbs that fan out one-per-lane during a /cold-eyes,
    // /indie-review, or /test-audit sweep. The canonical Phase-2 step
    // dispatches 12-16 briefs in one parallel batch, which the 10/min
    // Expensive cap was blocking. Cost shape is read-and-template-
    // assembly, not shell-out — semantically between Cheap and
    // Expensive. ANTS-1643 extended the tier to indie_review_brief +
    // test_audit_brief before the sibling fan-out report landed.
    if (toolName == QStringLiteral("cold_eyes_brief"))          return R::BriefAssembly;
    if (toolName == QStringLiteral("indie_review_brief"))       return R::BriefAssembly;
    if (toolName == QStringLiteral("test_audit_brief"))         return R::BriefAssembly;
    // Expensive — 10/min. Heavy verbs (shell-out, subagent dispatch,
    // cmake/ctest, full-corpus scan).
    if (toolName == QStringLiteral("audit_run"))                return R::Expensive;
    if (toolName == QStringLiteral("workspace_search"))         return R::Expensive;
    if (toolName == QStringLiteral("verify_changes"))           return R::Expensive;
    // ANTS-1302 — focused_test shells out to ctest.
    if (toolName == QStringLiteral("focused_test"))             return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_partition"))      return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_cross_doc_diff")) return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_fold_in"))        return R::Expensive;
    if (toolName == QStringLiteral("cold_eyes_single_doc"))     return R::Expensive;
    if (toolName == QStringLiteral("cross_doc_diff"))           return R::Expensive;
    // ANTS-1643: indie_review_brief + test_audit_brief moved to
    // BriefAssembly above. Their sibling partition / corroborate /
    // fold_in / dispatch verbs stay Expensive (heavier cost shapes —
    // subagent dispatch, multi-doc scan, file writes).
    if (toolName == QStringLiteral("indie_review_partition"))   return R::Expensive;
    if (toolName == QStringLiteral("indie_review_corroborate")) return R::Expensive;
    if (toolName == QStringLiteral("indie_review_fold_in"))     return R::Expensive;
    // ANTS-1279: indie_review_orchestrate derives the partition + N brief
    // manifests in one call — heavier than a single partition read.
    if (toolName == QStringLiteral("indie_review_orchestrate")) return R::Expensive;
    // ANTS-1352: indie_review_dispatch
    if (toolName == QStringLiteral("indie_review_dispatch"))    return R::Expensive;
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
        case RateLimitClass::BriefAssembly: {
            const int cap = (g_rateLimitBriefAssemblyCapOverride >= 0)
                ? g_rateLimitBriefAssemblyCapOverride
                : kRateLimitBriefAssemblyCap;
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

// ANTS-1771 — canonicalise a caller_cwd into a stable rate-limit bucket
// key. Existing paths resolve via canonicalFilePath (symlink + `.`/`..`
// + case on case-insensitive mounts); non-existent paths fall back to
// QDir::cleanPath (lexical-only) so synonyms still collapse. Empty in →
// empty out (the "absent caller_cwd" bucket).
QString ClaudeIntegration::canonicaliseCallerKey(const QString &callerCwd) {
    if (callerCwd.isEmpty()) return QString();
    const QString resolved = QFileInfo(callerCwd).canonicalFilePath();
    if (!resolved.isEmpty()) return resolved;
    return QDir::cleanPath(callerCwd);
}

qint64 ClaudeIntegration::rateLimitCheck(
    const QString &toolName, const QString &callerCwd, qint64 nowMs) {
    const RateLimitClass cls = rateLimitClassFor(toolName);
    if (cls == RateLimitClass::ControlPlane) return 0;
    const RateLimitTier tier = rateLimitTierFor(cls);

    // indie-review-2026-05-19 claudeintegration H1 + ANTS-1771 —
    // canonicalise the bucket key. Raw callerCwd values (`/foo`,
    // `/foo/`, `/foo/./`, `/foo//bar/..`, case-variants on
    // case-insensitive mounts) would otherwise produce distinct map
    // entries and let a same-UID peer multiply their effective budget
    // by routing through synonyms.
    //
    // ANTS-1771 — QFileInfo::canonicalFilePath returns "" for a
    // non-existent path; the pre-fix code fell back to the RAW string
    // there, so two non-existent synonyms (`/nope` vs `/nope/.`) still
    // landed in distinct buckets. We now fall back to QDir::cleanPath
    // (lexical normalisation: collapses `.`/`..`/`//`/trailing slash)
    // so the budget-multiplication hole closes for non-existent callers
    // too. The empty-string ("absent caller_cwd") bucket stays empty.
    //
    // Note: the tab-ROUTING path (ants::resolveCallerCwdRoot) is
    // deliberately NOT unified with this helper — routing must REJECT a
    // non-existent path (no tab can match it) rather than lexically
    // coerce it, so it keeps the existence-only canonicalFilePath rule.
    QString canon = canonicaliseCallerKey(callerCwd);
    const QPair<QString, QString> key{toolName, canon};
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

// ANTS-1629 — additive overload so tests can drive the new
// BriefAssembly tier without disturbing existing two-arg callers.
void ClaudeIntegration::setRateLimitCapsForTest(
    int cheap, int briefAssembly, int expensive) {
    g_rateLimitCheapCapOverride         = cheap;
    g_rateLimitBriefAssemblyCapOverride = briefAssembly;
    g_rateLimitExpensiveCapOverride     = expensive;
}

void ClaudeIntegration::resetRateLimitCapsForTest() {
    g_rateLimitCheapCapOverride         = -1;
    g_rateLimitBriefAssemblyCapOverride = -1;
    g_rateLimitExpensiveCapOverride     = -1;
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
    //
    // ANTS-1845: the QFileInfo::exists() probes run on paths built from a
    // `~/.claude/projects/<dir>` entry name. Those entries are written by
    // Claude Code under the user's own home (same UID), the probe count is
    // bounded by the hyphen count in the name, and this path is only
    // reached as a last resort (session metadata + transcript cwd both
    // failed). No write, no symlink follow — pure existence checks — so
    // the attacker-influence surface is negligible.
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

// ANTS-1670 M3 — an absolute path whose components contain no `..`
// traversal (and no NUL byte). Used to gate the untrusted transcript cwd
// before it is consumed as a process working directory / project root.
static bool isSafeAbsolutePath(const QString &p) {
    if (!QDir::isAbsolutePath(p)) return false;
    if (p.contains(QChar(u'\0'))) return false;
    const QStringList comps = p.split(QLatin1Char('/'));
    for (const QString &c : comps)
        if (c == QLatin1String("..")) return false;
    return true;
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
        const QString cwd = obj.value("cwd").toString();
        // ANTS-1670 M3 — the transcript is attacker-influenceable (a
        // corrupted or hand-crafted .jsonl), and this cwd flows on to
        // launch/resume a `claude` process working directory and a project
        // root. Accept only an absolute path with no `..` traversal; on a
        // bad value fall through to the caller's decodeProjectPath()
        // fallback. (Existence is deliberately NOT required — a validly
        // recorded project may have moved; a stale dir just fails the later
        // launch harmlessly, whereas a relative/`..` value is never valid.)
        if (!cwd.isEmpty() && isSafeAbsolutePath(cwd)) return cwd;
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
            // ANTS-2119 — session metadata is a few hundred bytes; skip an
            // implausibly large file rather than OOM on an uncapped readAll()
            // of a corrupt/hostile input.
            if (f.size() > 1 * 1024 * 1024) continue;
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
    // ANTS-1806 — cap per-line read (untrusted transcript, no file-size guard
    // on this path); a single huge line would OOM otherwise.
    constexpr qint64 kMaxLineBytes = 64 * 1024;
    int linesRead = 0;
    while (!file.atEnd() && linesRead < 50) {
        QByteArray line = file.readLine(kMaxLineBytes).trimmed();
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

