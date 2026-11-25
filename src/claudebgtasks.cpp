#include "claudebgtasks.h"
#include "claudecontent.h"   // ANTS-2002 — content-as-array text extraction
#include "claudetranscriptwalker.h"  // ANTS-1261 — shared JSONL walk + gating

#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
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

ClaudeBgTaskTracker::ClaudeBgTaskTracker(QObject *parent)
    : ClaudeTranscriptTrackerOf(parent) {}

QList<ClaudeBackgroundTask> ClaudeBgTaskTracker::parse(
        const QString &path, ClaudeTranscript::Cursor &cursor,
        ClaudeBgTaskAccum &acc) const {
    return parseIncremental(path, cursor, acc, m_processStartMs);
}

bool ClaudeBgTaskTracker::sameRow(const ClaudeBackgroundTask &a,
                                  const ClaudeBackgroundTask &b) const {
    return a.id == b.id && a.finished == b.finished;
}

bool ClaudeBgTaskTracker::otherSourcesMoved() {
    return walkHelperTranscripts(transcriptPath(), m_acc);
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
    if (transcriptPath().isEmpty()) return;
    if (replaceTasks(finalize(m_acc, m_processStartMs))) emit tasksChanged();
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
//      own text or an "enqueue" queue-operation record's content → end that id.
//   4a. `user` tool_result with success and toolUseResult.resumedAgentId
//      (SendMessage reviving a helper) → that agent runs again.
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
    const QString marker = QStringLiteral("written to: ");
    const int markerAt = resultText.indexOf(marker);
    if (markerAt < 0) return {};
    QString p = resultText.mid(markerAt + marker.size());
    const QString suffix = QStringLiteral(".output");
    const int suffixAt = p.indexOf(suffix);
    if (suffixAt >= 0) return p.left(suffixAt + suffix.size());
    p = p.trimmed();
    while (p.endsWith('.') || p.endsWith(' ')) p.chop(1);
    return p;
}

void finishById(ClaudeBgTaskAccum &acc, const QString &id) {
    if (id.isEmpty()) return;
    auto it = acc.idxByBgId.constFind(id);
    if (it != acc.idxByBgId.constEnd()) acc.raw[it.value()].finished = true;
    else acc.endedBeforeStart.insert(id);  // startTask applies it
}

// ANTS-5773 — end every task a <task-notification> block with a non-empty
// <status> names. A block without one (a monitor's per-event notice) ends
// nothing, except a monitor's expiry notice (ANTS-6404): Claude Code sends
// it with no <status>, only an "[Monitor expired after …]" event.
void applyNotifications(ClaudeBgTaskAccum &acc, const QString &text) {
    if (!text.contains(QLatin1String("<task-notification>"))) return;
    static const QRegularExpression block(
        QStringLiteral("<task-notification>(.*?)</task-notification>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression idRx(QStringLiteral("<task-id>\\s*([^<\\s]+)\\s*</task-id>"));
    static const QRegularExpression statusRx(QStringLiteral("<status>\\s*([^<]*?)\\s*</status>"));
    static const QRegularExpression expiredRx(QStringLiteral("<event>\\s*\\[Monitor expired after "));
    for (auto m = block.globalMatch(text); m.hasNext();) {
        const QString body = m.next().captured(1);
        const QString status = statusRx.match(body).captured(1);
        if (status.isEmpty() && !expiredRx.match(body).hasMatch()) continue;
        // ANTS-6322 — a helper pausing on its own background work is reported
        // "completed" too, and then carries on. Its end is a later notice, or
        // its meta file (walkHelperTranscripts).
        if (body.contains(QLatin1String("is waiting on its own background work"))) continue;
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
    if (acc.endedBeforeStart.remove(id)) t.finished = true;  // ANTS-6307
    acc.idxByBgId.insert(id, idx);
    return t;
}

// ANTS-6832 — SendMessage revived a helper whose end was already read, or one
// never tracked because it ran in the foreground. It runs again from the
// resume until its next end notice.
void resumeAgent(ClaudeBgTaskAccum &acc, const QString &tuId, const ClaudeBgPendingCall &call,
                 const QString &ts, const QString &id) {
    auto it = acc.idxByBgId.constFind(id);
    ClaudeBackgroundTask &t = it != acc.idxByBgId.constEnd()
        ? acc.raw[it.value()]
        : startTask(acc, tuId, call, ts, ClaudeBackgroundTask::Kind::Agent, id);
    if (t.tool.isEmpty()) t.tool = QStringLiteral("SendMessage");
    if (t.description.isEmpty()) t.description = id;
    t.finished = false;
    // From the resume, so the process-start rule (ANTS-5773 § 2.2) does not retire a
    // helper launched under an earlier Claude process.
    t.startedAt = QDateTime::fromString(ts, Qt::ISODateWithMs);
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
        // ANTS-6832 — a "remove" repeats a notice an "enqueue" already gave,
        // and would end a helper revived since.
        if (ev.value(QStringLiteral("operation")).toString() == QLatin1String("enqueue"))
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
            } else if (tur.value(QStringLiteral("success")).toBool()
                       && !tur.value(QStringLiteral("resumedAgentId")).toString().isEmpty()) {
                resumeAgent(acc, tuId, call, ts,
                            tur.value(QStringLiteral("resumedAgentId")).toString());
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

    // ANTS-6326 — a stopped helper with other work running may be paused on
    // that work, so it counts as running. The flag cannot say whose work it is;
    // once nothing else runs, it means the helper has ended.
    const bool otherWorkRuns = std::any_of(filtered.cbegin(), filtered.cend(),
        [](const ClaudeBackgroundTask &t) { return !t.finished && !t.helperStopped; });
    if (!otherWorkRuns) {
        for (auto &t : filtered)
            if (t.helperStopped) t.finished = true;
    }

    return filtered;
}

QList<ClaudeBackgroundTask> ClaudeBgTaskTracker::parseIncremental(
        const QString &path, ClaudeTranscript::Cursor &cursor,
        ClaudeBgTaskAccum &acc, qint64 processStartMs) {
    ClaudeTranscript::walkFrom(path, cursor,
        [&acc](const QJsonObject &ev, qint64) { applyBgEvent(acc, ev); });
    walkHelperTranscripts(path, acc);
    return finalize(acc, processStartMs);
}

// ANTS-6321 — one accumulator holds both sides, as a shell's end can be
// recorded in either transcript, and before its start is read (ANTS-6307).
bool ClaudeBgTaskTracker::walkHelperTranscripts(const QString &path, ClaudeBgTaskAccum &acc) {
    if (path.isEmpty()) return false;
    const QFileInfo session(path);
    const QDir folder(session.dir().filePath(session.completeBaseName() + QStringLiteral("/subagents")));
    const QFileInfoList helpers =
        folder.entryInfoList({QStringLiteral("agent-*.jsonl")}, QDir::Files);
    bool read = false;
    for (const QFileInfo &helper : helpers) {
        ClaudeTranscript::Cursor &cursor = acc.helperCursors[helper.filePath()];
        if (cursor.primed && helper.size() == cursor.offset) continue;
        if (!ClaudeTranscript::canResume(helper.filePath(), cursor)) cursor = {};
        ClaudeTranscript::walkFrom(helper.filePath(), cursor,
            [&acc](const QJsonObject &ev, qint64) { applyBgEvent(acc, ev); },
            ClaudeTranscript::Sidechains::Keep);
        read = true;
    }
    // ANTS-6322, ANTS-6326 — a helper's last notice can be a pause, so its meta
    // file is read too. Claude Code sets the flag at a pause as well as at the
    // end and clears it on resuming, so it is re-read rather than latched, and
    // finalize decides what it means. Running helpers only: this runs every 2 s.
    for (ClaudeBackgroundTask &t : acc.raw) {
        if (t.finished || t.kind != ClaudeBackgroundTask::Kind::Agent || t.id.isEmpty()) continue;
        QFile meta(folder.filePath(QStringLiteral("agent-%1.meta.json").arg(t.id)));
        const bool stopped = meta.open(QIODevice::ReadOnly)
            && QJsonDocument::fromJson(meta.readAll()).object()
                   .value(QStringLiteral("finished")).toBool();
        if (stopped != t.helperStopped) {
            t.helperStopped = stopped;
            read = true;
        }
    }
    return read;
}

QList<ClaudeBackgroundTask> ClaudeBgTaskTracker::parseTranscript(const QString &path) {
    // The full parse is the incremental one from a fresh cursor, so ANTS-5050
    // INV-8 holds by construction rather than by a second copy of the walk.
    ClaudeTranscript::Cursor cursor;
    ClaudeBgTaskAccum acc;
    return parseIncremental(path, cursor, acc);
}
