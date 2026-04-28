#include "claudebgtasks.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
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

} // namespace

ClaudeBgTaskTracker::ClaudeBgTaskTracker(QObject *parent) : QObject(parent) {
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &ClaudeBgTaskTracker::rescan);
}

ClaudeBgTaskTracker::~ClaudeBgTaskTracker() = default;

void ClaudeBgTaskTracker::setTranscriptPath(const QString &path) {
    if (path == m_transcriptPath) return;
    if (!m_transcriptPath.isEmpty())
        m_watcher.removePath(m_transcriptPath);
    m_transcriptPath = path;
    if (!m_transcriptPath.isEmpty() && QFileInfo::exists(m_transcriptPath))
        m_watcher.addPath(m_transcriptPath);
    rescan();
}

int ClaudeBgTaskTracker::runningCount() const {
    int n = 0;
    for (const auto &t : m_tasks) if (!t.finished) ++n;
    return n;
}

// 0.7.55 (2026-04-27 indie-review) — liveness sweep without reparse.
// Walks m_tasks and flips `finished` when the output file is gone or
// stale (mtime > 60 s old). Same staleness logic as parseTranscript's
// liveness pass, but skips the 16 MiB transcript walk — the status-
// bar 2 s timer calls this instead of rescan() so every tick costs
// N stat() calls rather than a full transcript read.
//
// Emits tasksChanged() only when at least one task's `finished` flag
// flipped, mirroring rescan()'s no-op-no-emit behaviour so a settled
// status bar doesn't repaint each tick.
void ClaudeBgTaskTracker::sweepLiveness() {
    constexpr qint64 kStaleSecs = 60;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    bool changed = false;
    for (auto &t : m_tasks) {
        if (t.finished) continue;
        if (t.outputPath.isEmpty()) continue;
        const QFileInfo fi(t.outputPath);
        if (!fi.exists()) {
            t.finished = true;
            changed = true;
            continue;
        }
        const QDateTime mtime = fi.lastModified().toUTC();
        if (mtime.secsTo(now) > kStaleSecs) {
            t.finished = true;
            changed = true;
        }
    }
    if (changed) emit tasksChanged();
}

void ClaudeBgTaskTracker::rescan() {
    QList<ClaudeBackgroundTask> next;
    if (!m_transcriptPath.isEmpty())
        next = parseTranscript(m_transcriptPath);

    // QFileSystemWatcher drops the path on atomic-rewrite; re-add if it
    // disappeared between rescans (Claude writes JSONL append-only, but
    // some editors do an atomic rename on save which trips the watcher).
    if (!m_transcriptPath.isEmpty()
            && QFileInfo::exists(m_transcriptPath)
            && !m_watcher.files().contains(m_transcriptPath)) {
        m_watcher.addPath(m_transcriptPath);
    }

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
}

// Pure parser. Walks the transcript line by line, accumulating a map of
// id → ClaudeBackgroundTask. Recognized events:
//
//   1. `assistant` with content[].type == "tool_use" and
//      input.run_in_background == true:
//      → start a partial entry keyed by tool_use_id (the "toolu_…" id).
//
//   2. `user` with content[].type == "tool_result" and
//      toolUseResult.backgroundTaskId set:
//      → finalize the entry by mapping tool_use_id → backgroundTaskId
//      and recording the output path (parsed from the tool_result text).
//
//   3. `user` with content[].type == "tool_result" whose
//      toolUseResult.status == "completed" / "killed":
//      → mark matching id finished. Look up by tool_use_id which for
//      BashOutput refs the original tool_use; we also accept matches by
//      bash_id / backgroundTaskId text-match where the tool_result body
//      mentions the id.
//
//   4. `assistant` with tool_use.name == "KillShell" referencing an id:
//      → mark that id finished.
//
// Out-of-scope (deliberate MVP limits):
//   • Cross-session correlation (each session is parsed in isolation).
//   • Recovering output paths after /tmp purge (we trust the transcript).
QList<ClaudeBackgroundTask> ClaudeBgTaskTracker::parseTranscript(const QString &path) {
    QList<ClaudeBackgroundTask> out;
    if (path.isEmpty()) return out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return out;

    // Cap parsed bytes at 16 MiB to bound work on enormous transcripts.
    // Background-task entries are short JSON objects and the fields we
    // care about are appended; truncating the head just loses old
    // already-finished tasks, which is acceptable.
    constexpr qint64 kMaxBytes = 16 * 1024 * 1024;
    const qint64 size = file.size();
    if (size > kMaxBytes) {
        if (!file.seek(size - kMaxBytes)) return out;
        // Discard the (likely-truncated) first line.
        file.readLine();
    }

    // Map tool_use_id (toolu_...) → index into `out`. Used to correlate
    // tool_use start with tool_result confirmation.
    QHash<QString, int> idxByToolUseId;
    // Map backgroundTaskId (e.g. "babmwvc5h") → index into `out`. Used
    // by completion/kill matchers.
    QHash<QString, int> idxByBgId;

    while (!file.atEnd()) {
        const QByteArray rawLine = file.readLine().trimmed();
        if (rawLine.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(rawLine);
        if (!doc.isObject()) continue;
        const QJsonObject ev = doc.object();
        const QString type = ev.value(QStringLiteral("type")).toString();

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

                // KillShell — finishes a tracked task by id.
                if (toolName == QLatin1String("KillShell")) {
                    const QString killId = input.value(QStringLiteral("shell_id")).toString();
                    auto it = idxByBgId.find(killId);
                    if (it != idxByBgId.end()) {
                        out[it.value()].finished = true;
                    }
                    continue;
                }

                // BackgroundTask launch.
                if (input.value(QStringLiteral("run_in_background")).toBool()) {
                    ClaudeBackgroundTask t;
                    t.tool = toolName;
                    t.description = input.value(QStringLiteral("description")).toString();
                    t.command = input.value(QStringLiteral("command")).toString();
                    if (t.command.isEmpty())
                        t.command = input.value(QStringLiteral("prompt")).toString();
                    t.command = squashOneLine(t.command, 400);
                    if (t.description.isEmpty()) {
                        // Task tool: description fallback to subagent_type.
                        t.description = input.value(QStringLiteral("subagent_type")).toString();
                    }
                    if (t.description.isEmpty()) t.description = t.command;
                    const QString ts = ev.value(QStringLiteral("timestamp")).toString();
                    t.startedAt = QDateTime::fromString(ts, Qt::ISODateWithMs);
                    out.append(t);
                    if (!toolUseId.isEmpty())
                        idxByToolUseId.insert(toolUseId, out.size() - 1);
                }
            }
        } else if (type == QLatin1String("user")) {
            const QJsonObject msg = ev.value(QStringLiteral("message")).toObject();
            const QJsonArray content = msg.value(QStringLiteral("content")).toArray();
            const QJsonObject tur = ev.value(QStringLiteral("toolUseResult")).toObject();
            const QString bgId = tur.value(QStringLiteral("backgroundTaskId")).toString();
            const QString trStatus = tur.value(QStringLiteral("status")).toString();
            for (const QJsonValue &cv : content) {
                const QJsonObject c = cv.toObject();
                if (c.value(QStringLiteral("type")).toString() != QLatin1String("tool_result"))
                    continue;
                const QString tuId = c.value(QStringLiteral("tool_use_id")).toString();

                // Confirmation of a launched background task.
                if (!bgId.isEmpty()) {
                    auto it = idxByToolUseId.find(tuId);
                    if (it != idxByToolUseId.end()) {
                        ClaudeBackgroundTask &t = out[it.value()];
                        t.id = bgId;
                        // The tool_result text is "Command running in
                        // background with ID: <id>. Output is being
                        // written to: <path>". Extract the path.
                        const QString resultText = c.value(QStringLiteral("content")).toString();
                        const int marker = resultText.indexOf(QStringLiteral("written to: "));
                        if (marker >= 0) {
                            QString p = resultText.mid(marker + 12).trimmed();
                            // Strip trailing punctuation if any.
                            while (p.endsWith('.') || p.endsWith(' ')) p.chop(1);
                            t.outputPath = p;
                        }
                        idxByBgId.insert(bgId, it.value());
                    }
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
                        const QString resultText = c.value(QStringLiteral("content")).toString();
                        for (auto it = idxByBgId.begin(); it != idxByBgId.end(); ++it) {
                            if (resultText.contains(it.key())) {
                                out[it.value()].finished = true;
                            }
                        }
                    }
                }
            }
        }
    }

    // Drop entries that never received a backgroundTaskId — those are
    // launches that crashed before the user-side confirmation, and we
    // can't display them meaningfully (no output path).
    QList<ClaudeBackgroundTask> filtered;
    filtered.reserve(out.size());
    for (auto &t : out) {
        if (!t.id.isEmpty()) filtered.append(std::move(t));
    }

    // Liveness sweep — bug 2026-04-27.
    // The transcript-based completion detection (KillShell event,
    // BashOutput tool_result with status=completed/killed/failed) only
    // catches tasks whose completion was *observed* by Claude Code via
    // a follow-up tool call. Background tasks that were spawned and
    // never polled, or whose completion landed after the assistant
    // moved on, leave entries with `finished == false` indefinitely —
    // resulting in a stale running-count chip on the status bar
    // (12-task report from a session with zero genuinely-running
    // tasks).
    //
    // Fix: cross-check each unfinished task against its on-disk output
    // file. Claude Code writes background-task stdout/stderr to
    // `/tmp/claude-$UID/<encoded-project>/<session>/tasks/<id>.output`
    // and stops touching the file the moment the underlying process
    // exits. We treat a task as finished when:
    //   • the file no longer exists (most likely /tmp purge after a
    //     reboot, but also fires if the file was reaped),
    //   • OR the file's mtime is older than the staleness window
    //     below (60 s of no writes ⇒ the producer is gone).
    //
    // The 60 s window is generous on purpose: a long-running build
    // can have 30+ seconds of silence between progress prints (CMake
    // configure step, slow link). 60 s only flips a task to finished
    // once we're confident no producer is attached. False-negatives
    // (real running task flagged finished) are corrected on the next
    // tick once the file does write again — but in practice the
    // assistant polls long-running tasks via BashOutput well before
    // the 60 s window elapses.
    constexpr qint64 kStaleSecs = 60;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto &t : filtered) {
        if (t.finished) continue;
        if (t.outputPath.isEmpty()) continue;  // can't tell — leave alone
        const QFileInfo fi(t.outputPath);
        if (!fi.exists()) {
            t.finished = true;
            continue;
        }
        const QDateTime mtime = fi.lastModified().toUTC();
        if (mtime.secsTo(now) > kStaleSecs) {
            t.finished = true;
        }
    }

    return filtered;
}
