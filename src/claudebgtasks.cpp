#include "claudebgtasks.h"
#include "claudetranscriptwatch.h"
#include "claudecontent.h"   // ANTS-2002 — content-as-array text extraction
#include "claudetranscriptwalker.h"  // ANTS-1261 — shared JSONL walk + gating

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

namespace {

// Truncate a command/prompt to a single short line for status display.
QString squashOneLine(QString s, int maxLen = 200) {
    s.replace('\n', ' ');
    s.replace('\t', ' ');
    while (s.contains(QStringLiteral("  "))) s.replace(QStringLiteral("  "), QStringLiteral(" "));
    s = s.trimmed();
    if (s.size() > maxLen) s = s.left(maxLen - 1) + QChar(0x2026); // …
    return s;
}

// ANTS-3516 — the one liveness rule both passes apply, so the parse-time
// pass and sweepLiveness() cannot drift apart: a task whose output file is
// gone has no producer attached. ANTS-5773 — silence is not an end: a shell
// waiting on a long job can write nothing for minutes. The phantoms the old
// 60 s window retired are now retired by the process-start rule (finalize).
bool outputGone(const QString &outputPath) {
    return !QFileInfo::exists(outputPath);
}

} // namespace

ClaudeBgTaskTracker::ClaudeBgTaskTracker(QObject *parent) : QObject(parent) {
    // ANTS-5050 — the watcher fires per append; the debounce collapses a
    // burst into one rescan instead of one full transcript walk each.
    m_rescanDebounce.setSingleShot(true);
    m_rescanDebounce.setInterval(kRescanDebounceMs);
    connect(&m_rescanDebounce, &QTimer::timeout, this, &ClaudeBgTaskTracker::rescan);
    // ANTS-1285 — one shared watch per transcript; filter on our own path.
    connect(&ClaudeTranscriptWatch::instance(), &ClaudeTranscriptWatch::changed,
            this, [this](const QString &path) {
                if (path == m_transcriptPath) m_rescanDebounce.start();
            });
}

ClaudeBgTaskTracker::~ClaudeBgTaskTracker() {
    if (auto *watch = ClaudeTranscriptWatch::existing())
        watch->release(m_transcriptPath);
}

void ClaudeBgTaskTracker::setTranscriptPath(const QString &path) {
    if (path == m_transcriptPath) return;
    ClaudeTranscriptWatch::instance().release(m_transcriptPath);
    m_transcriptPath = path;
    // Mirror claudetasklist: reset poll() change-signal shortcircuit so
    // a path re-bind always reparses.
    m_lastRescanMtimeMs   = 0;
    m_lastRescanSizeBytes = 0;
    // ANTS-5050 INV-6 — the cursor and accumulator belong to the OLD path.
    // INV-9 — take the state kept for the NEW path, if any.
    m_cursor = {};
    m_acc = {};
    walkCache().restore(m_transcriptPath, m_cursor, m_acc);
    ClaudeTranscriptWatch::instance().acquire(m_transcriptPath);
    m_rescanDebounce.stop();  // ANTS-5050 — this rescan supersedes a pending one
    rescan();
}

// ANTS-5050 INV-9 — one cache for every background-task tracker. One tracker
// exists per shell ever focused, so panes sharing a transcript share its state.
ClaudeTranscript::WalkCache<ClaudeBgTaskAccum> &ClaudeBgTaskTracker::walkCache() {
    static ClaudeTranscript::WalkCache<ClaudeBgTaskAccum> cache;
    return cache;
}

int ClaudeBgTaskTracker::runningCount() const {
    int n = 0;
    for (const auto &t : m_tasks) if (!t.finished) ++n;
    return n;
}

ClaudeBgPendingCounts ClaudeBgTaskTracker::pendingCounts(
        const QList<ClaudeBackgroundTask> &tasks) {
    ClaudeBgPendingCounts c;
    for (const auto &t : tasks) {
        if (t.finished) continue;
        switch (t.kind) {
            case ClaudeBackgroundTask::Kind::Shell:   ++c.shells; break;
            case ClaudeBackgroundTask::Kind::Agent:   ++c.agents; break;
            case ClaudeBackgroundTask::Kind::Monitor: ++c.monitors; break;
        }
    }
    return c;
}

// ANTS-5773 § 2.2 — re-applies the filters with the new start time, so a
// change takes effect without waiting for a transcript write.
void ClaudeBgTaskTracker::setProcessStartMs(qint64 ms) {
    if (ms == m_processStartMs) return;
    m_processStartMs = ms;
    if (m_transcriptPath.isEmpty()) return;
    QList<ClaudeBackgroundTask> next = finalize(m_acc, m_processStartMs);
    bool same = (next.size() == m_tasks.size());
    for (int i = 0; same && i < next.size(); ++i)
        same = next[i].id == m_tasks[i].id && next[i].finished == m_tasks[i].finished;
    m_tasks = std::move(next);
    if (!same) emit tasksChanged();
}

// 0.7.55 (2026-04-27 indie-review) — liveness sweep without reparse.
// Walks m_tasks and flips `finished` when the output file is gone. Same
// rule as parseTranscript's liveness pass, but skips the 16 MiB
// transcript walk — the status-
// bar 2 s timer calls this instead of rescan() so every tick costs
// N stat() calls rather than a full transcript read.
//
// Emits tasksChanged() only when at least one task's `finished` flag
// flipped, mirroring rescan()'s no-op-no-emit behaviour so a settled
// status bar doesn't repaint each tick.
void ClaudeBgTaskTracker::sweepLiveness() {
    bool changed = false;
    for (auto &t : m_tasks) {
        if (t.outputPath.isEmpty()) continue;
        // ANTS-1840 — a task flipped finished by THIS heuristic (not by an
        // authoritative transcript event) un-latches when its output file
        // reappears. Since poll() skips rescan() while the transcript mtime
        // is unchanged, this sweep is the only thing that re-derives
        // liveness. A transcript-finish (finishedByLiveness == false) is
        // permanent and never reconsidered here.
        if (t.finished) {
            if (!t.finishedByLiveness) continue;
            if (!outputGone(t.outputPath)) {
                t.finished = false;
                t.finishedByLiveness = false;
                changed = true;
            }
            continue;
        }
        if (outputGone(t.outputPath)) {
            t.finished = true;
            t.finishedByLiveness = true;
            changed = true;
        }
    }
    if (changed) emit tasksChanged();
}

void ClaudeBgTaskTracker::rescan() {
    // ANTS-1458 phase 2 — sample the change-signal (mtime + size) BEFORE
    // the parse read, so an append landing during the read leaves a newer
    // on-disk signal and the next poll() re-parses rather than stranding
    // it (mirror of ClaudeTaskListTracker::rescan).
    qint64 mtimeMs = 0, sizeBytes = 0;
    bool haveSignal = false;
    if (!m_transcriptPath.isEmpty()) {
        const QFileInfo fi(m_transcriptPath);
        if (fi.exists()) {
            mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
            sizeBytes = fi.size();
            haveSignal = true;
        }
    }

    QList<ClaudeBackgroundTask> next;
    if (!m_transcriptPath.isEmpty()) {
        // ANTS-5050 — resume from the cursor instead of re-walking the
        // 16 MiB tail. Cursor and accumulator reset together whenever the
        // cursor cannot be trusted; see ClaudeTranscript::canResume.
        if (!ClaudeTranscript::canResume(m_transcriptPath, m_cursor)) {
            m_cursor = {};
            m_acc = {};
        }
        next = parseIncremental(m_transcriptPath, m_cursor, m_acc, m_processStartMs);
        walkCache().store(m_transcriptPath, m_cursor, m_acc);  // ANTS-5050 INV-9
    }

    // QFileSystemWatcher drops the path on atomic-rewrite; re-add if it
    // disappeared between rescans (Claude writes JSONL append-only, but
    // some editors do an atomic rename on save which trips the watcher).
    ClaudeTranscriptWatch::instance().ensure(m_transcriptPath);

    // Compare shape: count, ids, finished flags. Skip emit if identical
    // — saves a UI rebuild when only metadata-only events appended.
    bool same = (next.size() == m_tasks.size());
    if (same) {
        for (int i = 0; i < next.size(); ++i) {
            if (next[i].id != m_tasks[i].id || next[i].finished != m_tasks[i].finished) {
                same = false;
                break;
            }
        }
    }
    m_tasks = std::move(next);
    if (!same) emit tasksChanged();

    // Store the pre-read baseline so poll() can short-circuit when
    // nothing has changed between ticks (ANTS-1458 phase 2).
    if (haveSignal) {
        m_lastRescanMtimeMs   = mtimeMs;
        m_lastRescanSizeBytes = sizeBytes;
    }
}

void ClaudeBgTaskTracker::poll() {
    if (m_transcriptPath.isEmpty()) return;
    const QFileInfo fi(m_transcriptPath);
    if (!fi.exists()) return;
    // File missing at bind time may now exist — re-add watch + reparse.
    if (ClaudeTranscriptWatch::instance().ensure(m_transcriptPath)) {
        rescan();
        return;
    }
    // ANTS-1458 phase 2 — re-parse when EITHER signal moved (mtime alone
    // misses a same-millisecond append; size catches it).
    const qint64 mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
    const qint64 sizeBytes = fi.size();
    if (mtimeMs == m_lastRescanMtimeMs && sizeBytes == m_lastRescanSizeBytes)
        return;
    rescan();
}

// Pure parser. Walks the transcript line by line, accumulating a map of
// id → ClaudeBackgroundTask. Recognized events (ANTS-5773 spec § 2.1):
//
//   1. `assistant` tool_use named Bash / Agent / Task / Monitor: remembered
//      by tool_use_id until its tool_result arrives. With
//      input.run_in_background it also starts a partial entry at once.
//   2. `user` tool_result for that call:
//      • toolUseResult.backgroundTaskId → a shell (also a foreground
//        command moved to the background on timeout, which has no flag);
//      • status "async_launched" + agentId → an agent;
//      • toolUseResult.taskId on a Monitor call → a monitor.
//      A flagged launch for the same tool_use_id takes the id rather than
//      a second task being started.
//   3. `user` tool_result whose toolUseResult.status is completed / killed
//      / failed → mark the matching id finished (BashOutput's era).
//   4. A <task-notification> with a non-empty <status>, in a user record's
//      own text or a queue-operation record's content → end that id.
//   5. `assistant` tool_use KillShell (shell_id) or TaskStop (task_id, or
//      the deprecated shell_id) → end that id.
//
// Out-of-scope (deliberate MVP limits):
//   • Cross-session correlation (each session is parsed in isolation).
//   • Recovering output paths after /tmp purge (we trust the transcript).
namespace {

// The shell's tool_result reads "... Output is being written to: <path>.
// You will be notified ..." — the path ends at its ".output" suffix. Older
// text ended at the path, so with no suffix the rest of the text is taken.
QString outputPathFromResult(const QString &resultText) {
    const int marker = resultText.indexOf(QStringLiteral("written to: "));
    if (marker < 0) return {};
    QString p = resultText.mid(marker + 12);
    const int suffix = p.indexOf(QStringLiteral(".output"));
    if (suffix >= 0) return p.left(suffix + 7);
    p = p.trimmed();
    while (p.endsWith('.') || p.endsWith(' ')) p.chop(1);
    return p;
}

void finishById(ClaudeBgTaskAccum &acc, const QString &id) {
    auto it = acc.idxByBgId.constFind(id);
    if (it != acc.idxByBgId.constEnd()) acc.raw[it.value()].finished = true;
}

// ANTS-5773 — end every task a <task-notification> block with a non-empty
// <status> names. A block without one (a monitor's per-event notice) ends
// nothing.
void applyNotifications(ClaudeBgTaskAccum &acc, const QString &text) {
    if (!text.contains(QLatin1String("<task-notification>"))) return;
    static const QRegularExpression block(
        QStringLiteral("<task-notification>(.*?)</task-notification>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression idRx(QStringLiteral("<task-id>\\s*([^<\\s]+)\\s*</task-id>"));
    static const QRegularExpression statusRx(QStringLiteral("<status>\\s*([^<]*?)\\s*</status>"));
    for (auto m = block.globalMatch(text); m.hasNext();) {
        const QString body = m.next().captured(1);
        const QString status = statusRx.match(body).captured(1);
        if (status.isEmpty()) continue;
        finishById(acc, idRx.match(body).captured(1));
    }
}

// ANTS-5773 — start a task from a remembered call, or give the id to the
// flagged launch already started for this tool_use_id.
ClaudeBackgroundTask &startTask(ClaudeBgTaskAccum &acc, const QString &tuId,
                                const ClaudeBgPendingCall &call,
                                const QString &fallbackTs,
                                ClaudeBackgroundTask::Kind kind,
                                const QString &id) {
    auto it = acc.idxByToolUseId.constFind(tuId);
    int idx;
    if (it != acc.idxByToolUseId.constEnd()) {
        idx = it.value();
    } else {
        ClaudeBackgroundTask t;
        t.tool = call.name;
        t.description = call.description.isEmpty() ? call.command : call.description;
        t.command = call.command;
        t.startedAt = call.startedAt.isValid()
            ? call.startedAt : QDateTime::fromString(fallbackTs, Qt::ISODateWithMs);
        acc.raw.append(t);
        idx = acc.raw.size() - 1;
        if (!tuId.isEmpty()) acc.idxByToolUseId.insert(tuId, idx);
    }
    ClaudeBackgroundTask &t = acc.raw[idx];
    t.id = id;
    t.kind = kind;
    acc.idxByBgId.insert(id, idx);
    return t;
}

// ANTS-5050 — fold one event into the accumulator. The full and incremental
// paths share this one implementation.
void applyBgEvent(ClaudeBgTaskAccum &acc, const QJsonObject &ev) {
    QList<ClaudeBackgroundTask> &out = acc.raw;
    QHash<QString, int> &idxByToolUseId = acc.idxByToolUseId;
    QHash<QString, int> &idxByBgId = acc.idxByBgId;

    const QString type = ev.value(QStringLiteral("type")).toString();
    const QString ts = ev.value(QStringLiteral("timestamp")).toString();

    if (type == QLatin1String("queue-operation")) {
        applyNotifications(acc, ev.value(QStringLiteral("content")).toString());
        return;
    }

    if (type == QLatin1String("assistant")) {
        const QJsonObject msg = ev.value(QStringLiteral("message")).toObject();
        const QJsonArray content = msg.value(QStringLiteral("content")).toArray();
        for (const QJsonValue &cv : content) {
            const QJsonObject c = cv.toObject();
            if (c.value(QStringLiteral("type")).toString() != QLatin1String("tool_use"))
                continue;
            const QString toolName = c.value(QStringLiteral("name")).toString();
            const QString toolUseId = c.value(QStringLiteral("id")).toString();
            const QJsonObject input = c.value(QStringLiteral("input")).toObject();

            // KillShell / TaskStop — finish a tracked task by id.
            if (toolName == QLatin1String("KillShell")
                || toolName == QLatin1String("TaskStop")) {
                QString killId = input.value(QStringLiteral("task_id")).toString();
                if (killId.isEmpty())
                    killId = input.value(QStringLiteral("shell_id")).toString();
                finishById(acc, killId);
                continue;
            }

            const bool mayStart = toolName == QLatin1String("Bash")
                || toolName == QLatin1String("Agent")
                || toolName == QLatin1String("Task")
                || toolName == QLatin1String("Monitor");
            const bool flagged = input.value(QStringLiteral("run_in_background")).toBool();
            if (!mayStart && !flagged) continue;

            ClaudeBgPendingCall call;
            call.name = toolName;
            call.description = input.value(QStringLiteral("description")).toString();
            call.command = input.value(QStringLiteral("command")).toString();
            if (call.command.isEmpty())
                call.command = input.value(QStringLiteral("prompt")).toString();
            call.command = squashOneLine(call.command, 400);
            if (call.description.isEmpty()) {
                // Task tool: description fallback to subagent_type.
                call.description = input.value(QStringLiteral("subagent_type")).toString();
            }
            call.startedAt = QDateTime::fromString(ts, Qt::ISODateWithMs);
            if (!toolUseId.isEmpty()) acc.calls.insert(toolUseId, call);

            // BackgroundTask launch.
            if (flagged) {
                ClaudeBackgroundTask t;
                t.tool = call.name;
                t.description = call.description.isEmpty() ? call.command : call.description;
                t.command = call.command;
                t.startedAt = call.startedAt;
                if (toolName == QLatin1String("Agent") || toolName == QLatin1String("Task"))
                    t.kind = ClaudeBackgroundTask::Kind::Agent;
                out.append(t);
                if (!toolUseId.isEmpty())
                    idxByToolUseId.insert(toolUseId, out.size() - 1);
            }
        }
    } else if (type == QLatin1String("user")) {
        const QJsonObject msg = ev.value(QStringLiteral("message")).toObject();
        const QJsonValue contentValue = msg.value(QStringLiteral("content"));
        // ANTS-5773 — a notification arrives as the user record's own text:
        // a string, or text blocks. Text inside a tool_result is a tool's
        // output (a file that quotes a notification) and is not read.
        if (contentValue.isString()) {
            applyNotifications(acc, contentValue.toString());
            return;
        }
        const QJsonArray content = contentValue.toArray();
        const QJsonObject tur = ev.value(QStringLiteral("toolUseResult")).toObject();
        const QString bgId = tur.value(QStringLiteral("backgroundTaskId")).toString();
        const QString trStatus = tur.value(QStringLiteral("status")).toString();
        for (const QJsonValue &cv : content) {
            const QJsonObject c = cv.toObject();
            const QString blockType = c.value(QStringLiteral("type")).toString();
            if (blockType == QLatin1String("text")) {
                applyNotifications(acc, c.value(QStringLiteral("text")).toString());
                continue;
            }
            if (blockType != QLatin1String("tool_result"))
                continue;
            const QString tuId = c.value(QStringLiteral("tool_use_id")).toString();
            const ClaudeBgPendingCall call = acc.calls.take(tuId);

            if (!bgId.isEmpty()) {
                // A shell: flagged, or moved to the background on timeout.
                ClaudeBackgroundTask &t = startTask(acc, tuId, call, ts,
                    ClaudeBackgroundTask::Kind::Shell, bgId);
                const QString p = outputPathFromResult(
                    ClaudeContent::toText(c.value(QStringLiteral("content"))));
                if (!p.isEmpty()) t.outputPath = p;
            } else if (trStatus == QLatin1String("async_launched")
                       && !tur.value(QStringLiteral("agentId")).toString().isEmpty()) {
                ClaudeBackgroundTask &t = startTask(acc, tuId, call, ts,
                    ClaudeBackgroundTask::Kind::Agent,
                    tur.value(QStringLiteral("agentId")).toString());
                t.outputPath = tur.value(QStringLiteral("outputFile")).toString();
            } else if (call.name == QLatin1String("Monitor")
                       && !tur.value(QStringLiteral("taskId")).toString().isEmpty()) {
                startTask(acc, tuId, call, ts, ClaudeBackgroundTask::Kind::Monitor,
                          tur.value(QStringLiteral("taskId")).toString());
            }

            // BashOutput / Task tool_result with a completion status.
            // The `tool_use_id` here points at the *BashOutput* call,
            // not the original launch — but the result text echoes
            // the bash_id, so we also do a substring scan over
            // running ids.
            const bool isComplete = (trStatus == QLatin1String("completed")
                                     || trStatus == QLatin1String("killed")
                                     || trStatus == QLatin1String("failed"));
            if (isComplete) {
                // Some payloads carry the id in toolUseResult.shellId
                // / toolUseResult.bash_id.
                QString idHint = tur.value(QStringLiteral("shellId")).toString();
                if (idHint.isEmpty())
                    idHint = tur.value(QStringLiteral("bash_id")).toString();
                if (!idHint.isEmpty()) {
                    auto it = idxByBgId.find(idHint);
                    if (it != idxByBgId.end()) {
                        out[it.value()].finished = true;
                    }
                } else {
                    // Fallback: scan result text for any tracked id.
                    // ANTS-1669 — match on a word boundary so a short id
                    // ("abc") isn't falsely flipped finished by a result
                    // that merely contains a longer id ("abc123") as a
                    // substring. Ids are escaped so regex metacharacters in
                    // an id are matched literally.
                    const QString resultText = ClaudeContent::toText(c.value(QStringLiteral("content")));
                    for (auto it = idxByBgId.begin(); it != idxByBgId.end(); ++it) {
                        // ANTS-1817 — cheap substring pre-filter before the
                        // word-boundary regex. Avoids compiling a fresh
                        // QRegularExpression per tracked id per completion
                        // event on every transcript-append rescan (the
                        // id isn't even present in the 99% no-match case).
                        if (!resultText.contains(it.key())) continue;
                        const QRegularExpression rx(
                            QStringLiteral("\\b") +
                            QRegularExpression::escape(it.key()) +
                            QStringLiteral("\\b"));
                        if (rx.match(resultText).hasMatch()) {
                            out[it.value()].finished = true;
                        }
                    }
                }
            }
        }
    }
}

}  // namespace

// ANTS-5050 — the end-of-parse filters. Re-runnable over the accumulator on
// every emit: the id filter cannot be baked in (the backgroundTaskId arrives
// in a later event than the launch), and the liveness pass reads the output
// files, so its answer legitimately changes between walks.
QList<ClaudeBackgroundTask> ClaudeBgTaskTracker::finalize(
        const ClaudeBgTaskAccum &acc, qint64 processStartMs) {

    // Drop entries that never received a backgroundTaskId — those are
    // launches that crashed before the user-side confirmation, and we
    // can't display them meaningfully (no output path).
    // ANTS-5050 — COPY, never move. `acc.raw` outlives this call and a later
    // append still needs every launch in it; moving out of it would empty the
    // accumulator on the first emit and lose every task on the next walk.
    QList<ClaudeBackgroundTask> filtered;
    filtered.reserve(acc.raw.size());
    for (const auto &t : acc.raw) {
        if (!t.id.isEmpty()) filtered.append(t);
    }

    // Liveness — bug 2026-04-27: tasks whose end was never recorded
    // lingered as phantoms on the running-count chip. Two rules retire
    // them (ANTS-5773 § 2.2):
    //   • a task started before this Claude process did belongs to an
    //     earlier process, and is not running. Old sessions' output files
    //     persist in /tmp, so "the file is gone" alone cannot retire them;
    //   • a task whose output file is gone has no producer (a /tmp purge
    //     after a reboot, or the file was reaped). sweepLiveness un-latches
    //     it if the file reappears.
    for (auto &t : filtered) {
        if (t.finished) continue;
        if (processStartMs > 0 && t.startedAt.isValid()
            && t.startedAt.toMSecsSinceEpoch() < processStartMs) {
            t.finished = true;
            continue;
        }
        if (t.outputPath.isEmpty()) continue;  // can't tell — leave alone
        if (outputGone(t.outputPath)) {
            t.finished = true;
            t.finishedByLiveness = true;  // ANTS-1840: heuristic, not transcript
        }
    }

    return filtered;
}

QList<ClaudeBackgroundTask> ClaudeBgTaskTracker::parseIncremental(
        const QString &path, ClaudeTranscript::Cursor &cursor,
        ClaudeBgTaskAccum &acc, qint64 processStartMs) {
    ClaudeTranscript::walkFrom(path, cursor,
        [&acc](const QJsonObject &ev, qint64) { applyBgEvent(acc, ev); });
    return finalize(acc, processStartMs);
}

QList<ClaudeBackgroundTask> ClaudeBgTaskTracker::parseTranscript(const QString &path) {
    // The full parse is the incremental one from a fresh cursor, so ANTS-5050
    // INV-8 holds by construction rather than by a second copy of the walk.
    ClaudeTranscript::Cursor cursor;
    ClaudeBgTaskAccum acc;
    return parseIncremental(path, cursor, acc);
}
