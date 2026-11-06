// ANTS-1677 claudeintegration piece 2/9 — Claude Code process detection, transcripts and project discovery
#include "claudeintegration.h"
#include "claudeintegration_internal.h"
#include "configpaths.h"
#include "debuglog.h"
#include "sessioncostverb.h"   // SessionCost::folderNameFor

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>

#include <algorithm>
#include <optional>
#include <unistd.h>

// ANTS-5089 — one JSONL transcript record. Tool results routinely exceed
// the old 64 KiB readLine cap, and a capped readLine hands the rest of the
// record back as further "lines" that fail to parse, so the record was
// dropped silently and each fragment counted against a caller's line budget.
// A record longer than `cap` is now skipped whole: its remainder is drained
// to the newline. The cap still bounds memory against a corrupt multi-GiB
// line (ANTS-1806).
static constexpr qint64 kMaxTranscriptRecordBytes = 16LL * 1024 * 1024;

// ANTS-6368 — read in small chunks: QIODevice::readLine(cap) allocates the
// whole cap on every call, which made each record cost a 16 MiB
// allocation. A record is kept when it and its newline fit in cap - 1
// bytes, the bound readLine(cap) had.
static QByteArray readJsonlRecord(QFile &file, qint64 cap) {
    QByteArray line;
    char chunk[16 * 1024];
    bool overCap = false;
    for (;;) {
        const qint64 n = file.readLine(chunk, sizeof chunk);
        if (n <= 0) break;
        if (!overCap) {
            if (line.size() + n > cap - 1) {
                overCap = true;  // drain to the newline; return nothing
                line = QByteArray();
            } else {
                line.append(chunk, n);
            }
        }
        if (chunk[n - 1] == '\n') break;
    }
    return line.trimmed();
}

// The last `window` bytes of a `size`-byte file. A window that starts
// mid-file drops its leading partial record; with no newline the whole
// buffer is that partial record, so the result is empty. nullopt when the
// seek fails.
static std::optional<QByteArray> readTailWindow(QFile &file, qint64 size,
                                                qint64 window) {
    const qint64 start = std::max<qint64>(0, size - window);
    if (!file.seek(start)) return std::nullopt;
    QByteArray tail = file.read(size - start);
    if (start > 0) {
        const int firstNl = tail.indexOf('\n');
        tail.remove(0, firstNl >= 0 ? firstNl + 1 : tail.size());
    }
    return tail;
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
        m_contextTokens = 0;
        m_contextEstimated = false;
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
        // prior tab's touched-file list to the new tab's queries.
        m_changedFiles.clear();
        emit stateChanged(ClaudeState::NotRunning, QString());
        emit contextUpdated(0);
    }
    m_shellPid = pid;
    m_orphanScanCountdown = 0;   // ANTS-5089 — a newly focused shell scans at once
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
pid_t ClaudeIntegration::findClaudeChildPid(pid_t shellPid, bool scanOrphans) {
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

    // ANTS-5089 — the caller asked for the fast path only.
    if (!scanOrphans) return 0;

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

    // ANTS-5089 — the orphan /proc scan reads every process's stat file.
    // While a Claude child is tracked it runs every poll, so an orphaned one
    // never reads as exited; with none tracked it runs once every
    // kOrphanScanEveryPolls polls, since a real shell's Claude child is found
    // on the fast path the moment it exists.
    const bool scanOrphans = m_claudePid != 0 || m_orphanScanCountdown <= 0;
    m_orphanScanCountdown = scanOrphans ? kOrphanScanEveryPolls - 1
                                        : m_orphanScanCountdown - 1;
    const pid_t foundPid = findClaudeChildPid(m_shellPid, scanOrphans);
    const bool found = foundPid > 0;

    if (!found) {
        // ANTS-1225-INV-3: when no Claude is running under the focused
        // shell, clear m_claudePid + m_transcriptPath and announce
        // NotRunning. This branch coexists with INV-1 — the != gate
        // below does NOT subsume the not-found case.
        m_claudePid = 0;
        m_transcriptPath.clear();
        // Unwatch the exited Claude's transcript too, so a later write to it
        // (a `claude --resume` in another tab) does not keep re-arming the
        // debounce for a parse the empty path then skips.
        if (const QStringList stale = m_transcriptWatcher.files(); !stale.isEmpty())
            m_transcriptWatcher.removePaths(stale);
        clearContextReading();   // ANTS-5683 — the reading was that Claude's
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
    // because setShellPid zeroes m_claudePid as a side effect.
    const bool pidChanged = (m_claudePid != foundPid);
    if (pidChanged) {
        // Newly detected Claude process (or replacement of a dead one).
        m_claudePid = foundPid;
        // ANTS-5092-INV-5 — the old transcript belongs to the Claude that
        // exited. Drop it, so an unresolved new one leaves the path empty and
        // the retry below keeps trying instead of reading a dead session.
        m_transcriptPath.clear();
        const QStringList watched = m_transcriptWatcher.files();
        if (!watched.isEmpty()) m_transcriptWatcher.removePaths(watched);
        // ANTS-5683 — and so does its reading. Kept, the meter showed the
        // old session's figure until the new session's first reply.
        clearContextReading();
    }

    // ANTS-4457 — resolve on a PID change AND whenever we still hold no
    // transcript. The PID is committed above whether or not the lookup
    // below succeeds, so before this the attempt was made exactly once:
    // a lookup returning empty left m_claudePid already equal to
    // foundPid, the branch never re-ran, and the backstop at the end of
    // this function is itself gated on m_transcriptPath being non-empty.
    // Returning empty on the first attempt is ordinary — /proc/<pid>/cwd
    // unreadable, no project directory yet on the first run in a repo,
    // or Claude started but not yet past its first transcript write,
    // which the process-anchored freshness filter rejects. The tab was
    // then stranded for the life of the process, treating every hook as
    // a cold start, recoverable only by switching tabs (setShellPid
    // zeroes m_claudePid). Retrying costs one /proc read and one scoped
    // directory listing per poll, and stops the moment it succeeds.
    if (pidChanged || m_transcriptPath.isEmpty()) {
        // ANTS-1168: scope to the focused tab's project rather than
        // walking ALL ~/.claude/projects entries. Without scoping, the
        // tab whose Claude process JUST started momentarily reads the
        // transcript for whichever other project happened to be most
        // recently active globally — same shape as ANTS-1163's stale-
        // session bug, which fixed `sessionPathForCwd` but not this
        // second site.
        // ANTS-5089 — Claude's own cwd first, the shell's as the fallback,
        // the order ClaudeTabTracker::detectClaudeChild uses. A Claude started
        // from a subshell after a `cd` runs in another project than the
        // shell's cwd, and resolving from the shell alone found no transcript.
        QString projectCwd =
            QFile::symLinkTarget(QString("/proc/%1/cwd").arg(m_claudePid));
        if (projectCwd.isEmpty())
            projectCwd =
                QFile::symLinkTarget(QString("/proc/%1/cwd").arg(m_shellPid));
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

    }

    // Set initial Idle state, then let transcript parse refine it.
    // ANTS-1225-INV-2: the m_state != Idle guard is mandatory —
    // removing it makes the status indicator flap on every poll
    // when steady-state Idle.
    // ANTS-4457 — stays on the PID change alone. This is about a newly
    // detected process, not about the transcript, so the retry condition
    // above must not reach it.
    if (pidChanged) {
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
// JSONL tail. Walks backwards from the last 32 KB, widening only when
// that holds no timestamped record, so we don't pay the full transcript
// scan on every refresh tick. Returns 0 when no
// timestamped event sits in the tail window — caller falls back to
// file mtime.
qint64 ClaudeIntegration::lastEventTimestampMs(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const qint64 size = f.size();

    // ANTS-5657 — a record can outgrow the window (a long tool result),
    // leaving no whole timestamped record in it. Widen fourfold and retry,
    // up to the per-record cap parseTranscriptTail uses (ANTS-5089).
    for (qint64 window = 32 * 1024;; window *= 4) {
        window = std::min(window, kMaxTranscriptRecordBytes);
        const std::optional<QByteArray> tail = readTailWindow(f, size, window);
        if (!tail) return 0;

        const QList<QByteArray> lines = tail->split('\n');
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
        if (window >= size || window >= kMaxTranscriptRecordBytes) return 0;
    }
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
            // Liveness floor (b). Tight when (a) is inactive (no PID known);
            // wide otherwise.
            const qint64 floor = (minLastEventMs > 0) ? kStaleMaxMsWithPid
                                                      : kStaleMaxMsNoPid;
            for (const QFileInfo &fi : proj.entryInfoList({"*.jsonl"}, QDir::Files, QDir::Time)) {
                // ANTS-5048 — newest mtime first, and a transcript's last
                // event is never later than its mtime (within the leeway).
                // Once a file's mtime can no longer pass a filter or beat the
                // best found, no later file can either: stop before reading.
                const qint64 mtimeCeil =
                    fi.lastModified().toMSecsSinceEpoch() + kLeewayMs;
                if (minLastEventMs > 0 && mtimeCeil < minLastEventMs - kLeewayMs)
                    break;
                if (nowMs > 0 && mtimeCeil < nowMs - floor) break;
                if (bestInfo.exists() && mtimeCeil <= bestEffMs) break;

                const qint64 effMs = effectiveLastEventMs(fi, minLastEventMs > 0);
                // Process-anchored identity filter (a).
                if (minLastEventMs > 0 && effMs < minLastEventMs - kLeewayMs)
                    continue;
                if (nowMs > 0 && effMs < nowMs - floor) continue;
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
    // ANTS-5092 — no cwd, no transcript. The unscoped newest `.jsonl` on the
    // machine is usually another tab's live session.
    if (projectCwd.isEmpty()) return {};
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

    // ANTS-1806 — cap per-record read. The transcript is written by an
    // external process (Claude Code) and is an untrusted parse boundary; a
    // corrupt multi-GiB single line would otherwise OOM the process even under
    // the whole-file guard above. ANTS-5089 — an over-cap record is skipped
    // whole rather than returned in fragments.
    while (!file.atEnd()) {
        QByteArray line = readJsonlRecord(file, kMaxTranscriptRecordBytes);
        if (line.isEmpty()) continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            entries.append(doc.object());
    }
    return entries;
}

QJsonArray ClaudeIntegration::loadTranscriptTail(const QString &path,
                                                 int maxEntries,
                                                 qint64 maxBytes,
                                                 int *totalRecords) const {
    QJsonArray entries;
    int total = 0;
    const auto finish = [&]() {
        if (totalRecords) *totalRecords = total;
        return entries;
    };
    QFile file(path);
    if (maxEntries <= 0 || !file.open(QIODevice::ReadOnly)) return finish();
    // Same whole-file cap as loadTranscript.
    if (file.size() > 100LL * 1024 * 1024) return finish();

    // Pass 1: walk record boundaries, keeping only the offsets of the last
    // maxEntries non-empty records. Nothing is parsed here.
    QList<qint64> starts;
    while (!file.atEnd()) {
        const qint64 pos = file.pos();
        if (readJsonlRecord(file, kMaxTranscriptRecordBytes).isEmpty()) continue;
        ++total;
        starts.append(pos);
        if (starts.size() > maxEntries) starts.removeFirst();
    }
    // ANTS-5092 — drop the oldest kept records until the rest fit in
    // maxBytes; the newest record stays whatever its size.
    const qint64 end = file.pos();
    while (starts.size() > 1 && end - starts.first() > maxBytes)
        starts.removeFirst();
    // Pass 2: parse from the oldest kept record to the end.
    if (starts.isEmpty() || !file.seek(starts.first())) return finish();
    while (!file.atEnd()) {
        const QByteArray line = readJsonlRecord(file, kMaxTranscriptRecordBytes);
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) entries.append(doc.object());
    }
    return finish();
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

// ANTS-5682 — the text a user record adds to the context: a typed prompt, or
// the text blocks and tool results it carries. Images are skipped, since
// their token cost is not proportional to their bytes. The record's other
// fields (toolUseResult repeats the tool's output) are not context.
static qint64 userRecordTextBytes(const QJsonObject &record) {
    const QJsonValue content = record.value("message").toObject().value("content");
    if (content.isString()) return content.toString().toUtf8().size();
    qint64 bytes = 0;
    const auto textOf = [](const QJsonValue &v) -> qint64 {
        if (v.isString()) return v.toString().toUtf8().size();
        qint64 n = 0;
        for (const QJsonValue &b : v.toArray())
            n += b.toObject().value("text").toString().toUtf8().size();
        return n;
    };
    for (const QJsonValue &c : content.toArray()) {
        const QJsonObject block = c.toObject();
        const QString type = block.value("type").toString();
        if (type == QLatin1String("text"))
            bytes += block.value("text").toString().toUtf8().size();
        else if (type == QLatin1String("tool_result"))
            bytes += textOf(block.value("content"));
    }
    return bytes;
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
    // ANTS-5089: the cap is one byte past the record cap readJsonlRecord
    // uses, so the newline before a record of up to that size is inside the
    // window. At the old 4 MiB cap a larger final record left no event in the
    // window and the status froze. The window only grows this far while the
    // last record is that large.
    constexpr qint64 kMaxWindow = kMaxTranscriptRecordBytes + 1;
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
            // ANTS-1169: a single tool_result that exceeds the
            // window (a record over the ANTS-5089 record cap) used to
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
    const auto parseEvents = [](const QByteArray &buf) {
        QList<QJsonObject> out;
        for (const QByteArray &raw : buf.split('\n')) {
            const QByteArray line = raw.trimmed();
            if (line.isEmpty()) continue;
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) continue;
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("isSidechain")).toBool()) continue;
            out.append(obj);
        }
        return out;
    };
    const QList<QJsonObject> events = parseEvents(tail);
    // The window `events` came from. The context reading below widens
    // `window` for itself; the state search widens its own copy.
    qint64 stateWindow = window;

    if (events.isEmpty()) return snap;
    snap.hasEvents = true;

    // Context size from the most recent event carrying usage info. Walk
    // backward. With prompt caching, input_tokens is only the uncached tail,
    // so the context is the sum of all three input fields.
    // ANTS-5631 — a compact boundary newer than every usage event is the
    // reading: Claude Code records the post-compact size on it.
    const auto readContext = [&snap](const QList<QJsonObject> &evs) {
        for (int i = evs.size() - 1; i >= 0; --i) {
            const QJsonObject usage = evs[i].value("message").toObject()
                                            .value("usage").toObject();
            qint64 tokens =
                usage.value("input_tokens").toInteger()
                + usage.value("cache_creation_input_tokens").toInteger()
                + usage.value("cache_read_input_tokens").toInteger();
            const bool isCompact = evs[i].value("subtype").toString()
                    == QLatin1String("compact_boundary");
            if (isCompact)
                tokens = evs[i].value("compactMetadata").toObject()
                             .value("postTokens").toInteger();
            if (tokens > 0 && !isCompact) {
                // ANTS-5682 — the reply itself joins the context, and so
                // does every user or tool-result record written after it.
                // Those carry no usage, so estimate them at four bytes a
                // token until the next reply reports the real figure.
                tokens += usage.value("output_tokens").toInteger();
                qint64 laterBytes = 0;
                for (int j = i + 1; j < evs.size(); ++j)
                    if (evs[j].value("type").toString() == QLatin1String("user"))
                        laterBytes += userRecordTextBytes(evs[j]);
                if (laterBytes > 0) {
                    tokens += laterBytes / 4;
                    snap.contextEstimated = true;
                }
            }
            if (tokens > 0) {
                snap.contextTokens = tokens;
                snap.contextPercent = static_cast<int>(std::min<qint64>(
                    100, tokens * 100 / kDefaultContextWindowTokens));
                return true;
            }
        }
        return false;
    };
    // ANTS-5731 — the window above stops at two newlines, and a session can
    // end with more metadata than that (attachments, last-prompt, mode
    // records). Keep doubling, for the context reading only, until a usage
    // record or compact boundary is in the window.
    if (!readContext(events)) {
        while (window < kMaxWindow && window < size) {
            window = std::min(window * 2, kMaxWindow);
            const std::optional<QByteArray> wider =
                readTailWindow(file, size, window);
            if (!wider) break;
            if (readContext(parseEvents(*wider))) break;
        }
    }

    // ANTS-5779 — only these record types say what Claude is doing. Claude
    // Code writes a growing set of bookkeeping records after a turn's real
    // event (last-prompt, ai-title, mode, atis-latch, api-request, …), so the
    // walk skips everything not on this list, rather than a list of the
    // bookkeeping types, which went stale and froze the state.
    // ANTS-6306 — a local command (/mcp, /model) starts no turn. Its caveat,
    // <command-name> and output records share one promptId, and none of them
    // decides state; a skill's /command has no output record, so it still does.
    // ANTS-6323 — an attachment only ever accompanies a turn's real records, and
    // a compact summary starts no turn: an idle session compacts itself and
    // writes one, then attachments, then nothing until the user types.
    const auto isLocalCommandOutput = [](const QJsonObject &ev) {
        return ev.value("message").toObject().value("content").toString()
                   .startsWith(QLatin1String("<local-command-"));
    };
    const auto findStateEvent = [&isLocalCommandOutput](const QList<QJsonObject> &evs) {
        QSet<QString> localPrompts;
        for (const QJsonObject &ev : evs) {
            const QString promptId = ev.value("promptId").toString();
            if (!promptId.isEmpty() && isLocalCommandOutput(ev))
                localPrompts.insert(promptId);
        }
        for (int i = evs.size() - 1; i >= 0; --i) {
            const QString t = evs[i].value("type").toString();
            if (t == QLatin1String("user") &&
                (isLocalCommandOutput(evs[i]) ||
                 localPrompts.contains(evs[i].value("promptId").toString())))
                continue;
            if (evs[i].value("isCompactSummary").toBool()) continue;
            if (t == QLatin1String("assistant") || t == QLatin1String("user") ||
                t == QLatin1String("human"))
                return evs[i];
        }
        return QJsonObject();
    };
    QJsonObject stateEvent = findStateEvent(events);
    // Bookkeeping can outgrow the window (an api-request-blob runs to
    // hundreds of KiB). Widen it, as for the context reading above, until a
    // state record is inside.
    while (stateEvent.isEmpty() && stateWindow < kMaxWindow && stateWindow < size) {
        stateWindow = std::min(stateWindow * 2, kMaxWindow);
        const std::optional<QByteArray> wider =
            readTailWindow(file, size, stateWindow);
        if (!wider) break;
        stateEvent = findStateEvent(parseEvents(*wider));
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
        }
    }

    // /compact override. The command is recorded in the transcript as a user
    // event with string content "<command-name>/compact</command-name>...".
    // Compaction completes when Claude writes another user event carrying the
    // condensed history with `isCompactSummary:true`. While the former is the
    // most recent real user message and no matching summary has followed,
    // surface Compacting — otherwise the UI just says "thinking..." for the
    // entire (often multi-second) summarization turn. Walks the same tail
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
    // Important: we parse only a tail window, so a toggle that
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

void ClaudeIntegration::setContextWindowTokens(qint64 tokens) {
    if (tokens <= 0 || tokens == m_contextWindowTokens) return;
    m_contextWindowTokens = tokens;
    if (m_contextTokens <= 0) return;
    m_contextPercent = static_cast<int>(std::min<qint64>(
        100, m_contextTokens * 100 / m_contextWindowTokens));
    emit contextUpdated(m_contextPercent);
}

void ClaudeIntegration::clearContextReading() {
    if (m_contextTokens <= 0 && m_contextPercent == 0) return;
    m_contextTokens = 0;
    m_contextPercent = 0;
    m_contextEstimated = false;
    emit contextUpdated(0);
}

void ClaudeIntegration::seedContextReading(qint64 tokens, bool estimated) {
    if (tokens <= 0) return;
    m_contextTokens = tokens;
    m_contextEstimated = estimated;
    m_contextPercent = static_cast<int>(std::min<qint64>(
        100, m_contextTokens * 100 / m_contextWindowTokens));
    emit contextUpdated(m_contextPercent);
}

void ClaudeIntegration::parseTranscriptForState(const QString &path) {
    // Re-add the watch — QFileSystemWatcher can drop it after atomic saves
    if (!m_transcriptWatcher.files().contains(path))
        m_transcriptWatcher.addPath(path);

    const ClaudeTranscriptSnapshot snap = parseTranscriptTail(path, m_planMode);
    if (!snap.hasEvents) return;

    if (snap.contextTokens > 0) {
        m_contextTokens = snap.contextTokens;
        m_contextEstimated = snap.contextEstimated;
        m_contextPercent = static_cast<int>(std::min<qint64>(
            100, m_contextTokens * 100 / m_contextWindowTokens));
        emit contextUpdated(m_contextPercent);
    }

    if (!snap.toolUseBlock.isEmpty()) {
        // Transcript dialect: a tool_use content block spells them name/input.
        updateChangedFiles(snap.toolUseBlock.value("name").toString(),
                           snap.toolUseBlock.value("input").toObject());
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

bool ClaudeIntegration::isSecretFileName(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    return name.startsWith(QLatin1String(".env"))
        || name.endsWith(QLatin1String(".pem"))
        || name.startsWith(QLatin1String("id_"));
}

void ClaudeIntegration::updateChangedFiles(const QString &toolName,
                                           const QJsonObject &toolInput) {
    QString filePath;
    if (toolName == "Edit" || toolName == "Write" || toolName == "Read") {
        filePath = toolInput.value("file_path").toString();
    } else if (toolName == "Bash") {
        // Can't reliably extract file paths from bash commands
        return;
    }

    // ANTS-5169 — every read of a secrets file warns, so this sits ahead
    // of the once-per-path check below.
    if (toolName == "Read" && isSecretFileName(filePath))
        emit sensitiveFileRead(filePath);

    if (!filePath.isEmpty() && !m_changedFiles.contains(filePath)) {
        m_changedFiles.append(filePath);
        if (m_changedFiles.size() > 50)
            m_changedFiles.removeFirst();
        emit fileChanged(filePath);
    }
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
    // ANTS-4457 — the value returned here is published by the caller as a
    // project root, so it must not be a traversal or a relative path. The
    // ANTS-1845 assessment above covers the existence PROBING and stands;
    // it is not an argument about the value. A name that is not an encoded
    // absolute path, or that decodes to a `..` component, has no safe
    // answer — return empty and let the caller's existing
    // empty-means-unresolved branch deal with it.
    if (!encoded.startsWith('-')) return {};
    const QStringList tokens = encoded.mid(1).split('-');
    if (tokens.isEmpty() || tokens.first().isEmpty()) return QStringLiteral("/");
    for (const QString &t : tokens)
        if (t == QLatin1String("..")) return {};

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
    // Claude Code's folder name under ~/.claude/projects/. Every character
    // outside [A-Za-z0-9] folds to '-': `_` (ANTS-1192) and `.` (ANTS-5317)
    // each hid a session once. One rule, shared with session_cost and
    // run_trace.
    return SessionCost::folderNameFor(path);
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

    // 0.7.52 (2026-04-27 indie-review HIGH) — cap the per-record read. The
    // transcript is a JSONL file written by Claude Code; pathological /
    // corrupted input could put a multi-GiB single line at the head and
    // OOM the process before we get to the early-return cap. ANTS-5089 —
    // an over-cap record is skipped whole, so it costs one of the 30 records
    // rather than one per fragment.
    int linesRead = 0;
    while (!file.atEnd() && linesRead < 30) {
        QByteArray line = readJsonlRecord(file, kMaxTranscriptRecordBytes);
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
            // ANTS-4457 — gate this the way ANTS-1670 M3 gates the
            // transcript's cwd. Same threat model, same consumption as a
            // process working directory and a project root — and this one
            // is tried FIRST, so leaving it ungated meant the checked
            // source never ran when both were present. Checking at store
            // time keeps the map safe by construction, so an unsafe value
            // falls through to the next source instead of shadowing it.
            if (!cwd.isEmpty() && isSafeAbsolutePath(cwd)) sessionCwds[sid] = cwd;
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
        // ANTS-4457 — one gate over the resolved value, whichever of the
        // three sources produced it, so a source added later inherits it.
        // A project whose path cannot be trusted is skipped rather than
        // published with a path nobody validated.
        if (!isSafeAbsolutePath(realPath)) continue;
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

            // Summaries load here for the first 5 sessions per project;
            // the dialog loads the rest on demand.
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
    // ANTS-1806 — cap per-record read (untrusted transcript, no file-size
    // guard on this path); a single huge line would OOM otherwise. ANTS-5089 —
    // a first user message longer than 64 KiB (a pasted file) is still found.
    int linesRead = 0;
    while (!file.atEnd() && linesRead < 50) {
        QByteArray line = readJsonlRecord(file, kMaxTranscriptRecordBytes);
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

    // Only the first 500 chars are kept, and 500 UTF-16 units never take
    // more than 2000 UTF-8 bytes, so a bounded read decodes all of them
    // instead of loading a large memory file whole.
    QString content = QString::fromUtf8(file.read(4096));
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
