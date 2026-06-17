#include "claudetasklist.h"
#include "claudecontent.h"   // ANTS-2002 — content-as-array text extraction

#include <algorithm>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

// Best-effort task id from a tool_result body. The authoritative pairing
// of a result to its TaskCreate is the structured `tool_use_id` (handled
// by the caller via idxByToolUseId); this only recovers the human-facing
// numeric id ("#N") that later TaskUpdate events reference as `taskId`.
//
// ANTS-1840 — the id lives in prose only ("Task #N created successfully:"),
// so the match is deliberately tolerant of cosmetic wording drift:
// case-insensitive and whitespace-flexible around "Task" / "#" (covers
// "Task #5", "task#5", "Task # 5"). The "Task" anchor is kept so a bare
// "#5" inside a task subject can't be mis-captured. A miss leaves the id
// empty, which is fine — id is only used for TaskUpdate matching, not
// display.
QString extractIdFromResultBody(const QString &body) {
    static const QRegularExpression re(
        QStringLiteral(R"(Task\s*#\s*(\d+))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(body);
    return m.hasMatch() ? m.captured(1) : QString{};
}

// Build a ClaudeTask from a `todos[]` element of a TodoWrite snapshot.
// The TodoWrite shape uses `content` (not `subject`) for the title.
ClaudeTask taskFromTodoEntry(const QJsonObject &o) {
    ClaudeTask t;
    t.subject    = o.value(QStringLiteral("content")).toString();
    t.activeForm = o.value(QStringLiteral("activeForm")).toString();
    t.status     = o.value(QStringLiteral("status")).toString();
    if (t.status.isEmpty()) t.status = QStringLiteral("pending");
    return t;
}

// ANTS-1341: parse an ISO-8601-with-ms timestamp string into epoch
// ms. Returns 0 on empty / unparseable input — fail-soft so the
// abandonment filter preserves tasks whose timestamps are missing.
// Pattern matches src/claudebgtasks.cpp:245-246.
qint64 parseIsoMs(const QString &ts) {
    if (ts.isEmpty()) return 0;
    const QDateTime dt = QDateTime::fromString(ts, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

// ANTS-1341: abandonment threshold. An `in_progress` task whose
// last event is more than 24 h before the latest event in the
// transcript is dropped as abandoned (subagent crashed, partial
// network failure, transcript-replay artefact). Other statuses
// (pending = unstarted backlog, completed = history) are NOT
// time-bounded.
constexpr qint64 kStaleInProgressMs = 24LL * 3600LL * 1000LL;

} // namespace

ClaudeTaskListTracker::ClaudeTaskListTracker(QObject *parent)
    : QObject(parent) {
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &ClaudeTaskListTracker::rescan);
}

ClaudeTaskListTracker::~ClaudeTaskListTracker() = default;

void ClaudeTaskListTracker::setTranscriptPath(const QString &path) {
    if (path == m_transcriptPath) return;
    if (!m_transcriptPath.isEmpty())
        m_watcher.removePath(m_transcriptPath);
    m_transcriptPath = path;
    // Reset the poll() change-signal shortcircuit — if the user rebinds
    // the same project shortly after a clear (tab-switch round trip) and
    // the file mtime/size hasn't ticked, poll() would otherwise skip the
    // legitimate rescan.
    m_lastRescanMtimeMs   = 0;
    m_lastRescanSizeBytes = 0;
    if (!m_transcriptPath.isEmpty() && QFileInfo::exists(m_transcriptPath))
        m_watcher.addPath(m_transcriptPath);
    rescan();
}

int ClaudeTaskListTracker::unfinishedCount() const {
    // ANTS-1221 — pending only. The chip surfaces user-actionable
    // work, and "Claude is currently running it" is not actionable
    // by the user. ANTS-1216 originally widened this to
    // `pending + in_progress` to match the dialog's "outstanding"
    // wording, but the user reported (2026-05-10 screenshot:
    // "41 tasks — 40 done, 1 running, 0 outstanding" → chip read
    // "☰ 1/41") that the chip kept lingering on a single in-flight
    // task with nothing left for them to action. Splitting the
    // contract: chip = pending; dialog header still splits running
    // from outstanding. Pairs with ANTS-1218 (chip numerator flipped
    // to `total - unfinished`, so the chip counts up and hides
    // cleanly at 100% via the existing `unfinished <= 0` branch).
    return pendingCount();
}

int ClaudeTaskListTracker::inProgressCount() const {
    int n = 0;
    for (const auto &t : m_tasks) {
        if (t.status == QStringLiteral("in_progress")) ++n;
    }
    return n;
}

int ClaudeTaskListTracker::pendingCount() const {
    int n = 0;
    for (const auto &t : m_tasks) {
        if (t.status == QStringLiteral("pending")) ++n;
    }
    return n;
}

int ClaudeTaskListTracker::completedCount() const {
    int n = 0;
    for (const auto &t : m_tasks) {
        if (t.status == QStringLiteral("completed")) ++n;
    }
    return n;
}

void ClaudeTaskListTracker::rescan() {
    // ANTS-1458 phase 2 — sample the change-signal (mtime + size) BEFORE
    // the parse, not after. parseTranscript() reads to the file's current
    // EOF; if Claude appends during that read window (common in a
    // TaskCreate burst — it writes every ~100 ms), recording the signal
    // afterward would capture the post-append mtime/size while the parse
    // missed those bytes. poll()'s equality short-circuit would then
    // strand the new task until an unrelated later write advanced the
    // signal again — the user-reported >2 s lag. Sampling the pre-read
    // baseline means any write landing during/after the read leaves a
    // strictly newer on-disk signal, so the next poll() re-parses (at
    // worst one redundant rescan).
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

    QList<ClaudeTask> next;
    if (!m_transcriptPath.isEmpty())
        next = parseTranscript(m_transcriptPath);

    // QFileSystemWatcher drops the path on atomic-rewrite; re-add if
    // it disappeared between rescans. Same shape as
    // claudebgtasks.cpp:91-95.
    if (!m_transcriptPath.isEmpty()
            && QFileInfo::exists(m_transcriptPath)
            && !m_watcher.files().contains(m_transcriptPath)) {
        m_watcher.addPath(m_transcriptPath);
    }

    // Compare shape: same length AND same (id, status) pairs in the
    // same order. Skip emit if identical to avoid UI churn.
    bool same = (next.size() == m_tasks.size());
    if (same) {
        for (int i = 0; i < next.size(); ++i) {
            if (next[i].id != m_tasks[i].id
                || next[i].status != m_tasks[i].status
                || next[i].subject != m_tasks[i].subject) {
                same = false;
                break;
            }
        }
    }
    m_tasks = std::move(next);
    if (!same) emit tasksChanged();

    // Store the pre-read baseline so poll() can short-circuit when
    // nothing has changed between ticks (ANTS-1458 phase 2 — both fields
    // sampled above, before the parse).
    if (haveSignal) {
        m_lastRescanMtimeMs   = mtimeMs;
        m_lastRescanSizeBytes = sizeBytes;
    }
}

void ClaudeTaskListTracker::poll() {
    if (m_transcriptPath.isEmpty()) return;
    const QFileInfo fi(m_transcriptPath);
    if (!fi.exists()) return;
    // ANTS-2002 — parity with ClaudeBgTaskTracker::poll(): the watch may have
    // been lost (file absent at bind time, atomic-rename on save, or inotify
    // exhaustion). Re-add it and reparse so a dropped watch can't strand the
    // task list on stale state until the next mtime tick happens to differ.
    if (!m_watcher.files().contains(m_transcriptPath)) {
        m_watcher.addPath(m_transcriptPath);
        rescan();
        return;
    }
    // ANTS-1458 phase 2 — re-parse when EITHER signal moved. mtime alone
    // misses a same-millisecond append (coarse-granularity filesystem, or
    // a burst landing inside one mtime tick); size catches it.
    const qint64 mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
    const qint64 sizeBytes = fi.size();
    if (mtimeMs == m_lastRescanMtimeMs && sizeBytes == m_lastRescanSizeBytes)
        return;
    rescan();
}

qint64 ClaudeTaskListTracker::lastRescanMtimeMs() const {
    return m_lastRescanMtimeMs;
}

// Pure parser. Walks the transcript line by line. See header for
// event shapes; full disambiguation table at docs/specs/ANTS-1158.md
// §3.1.
//
// Mode A (TodoWrite snapshot): if any TodoWrite event is seen, the
//   most recent one wins — full replace. Walking forward and
//   overwriting `out` on each TodoWrite achieves this naturally
//   without a separate backward-EOF-walk pass.
//
// Mode B (TaskCreate / TaskUpdate replay): when no TodoWrite was
//   ever seen, accumulate from TaskCreate and apply TaskUpdate
//   status flips by taskId.
//
// Filters:
//   * Events with isSidechain == true skipped (subagent inline turns).
//   * Task tool_use with `subagent_type` filtered (subagent dispatch,
//     not a plan add).
QList<ClaudeTask> ClaudeTaskListTracker::parseTranscript(const QString &path) {
    QList<ClaudeTask> out;
    if (path.isEmpty()) return out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return out;

    // Cap parsed bytes at 16 MiB — same bound as claudebgtasks.cpp:147.
    // Mode B truncation drops oldest TaskCreate history (acceptable).
    // Mode A truncation could in theory drop the only TodoWrite — but
    // a 16 MiB+ session is extreme and the latest snapshot typically
    // sits in the recent tail. If a user hits this we'll re-evaluate.
    constexpr qint64 kMaxBytes = 16 * 1024 * 1024;
    const qint64 size = file.size();
    if (size > kMaxBytes) {
        if (!file.seek(size - kMaxBytes)) return out;
        file.readLine();  // discard partial leading line
    }

    bool sawTodoWrite = false;
    QHash<QString, int> idxByToolUseId;   // tool_use_id → index in `out`
    qint64 latestEventMs = 0;             // ANTS-1341: monotonic over the
                                          // walk; abandonment threshold uses
                                          // this as the reference time

    while (!file.atEnd()) {
        const QByteArray rawLine = file.readLine().trimmed();
        if (rawLine.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(rawLine);
        if (!doc.isObject()) continue;
        const QJsonObject ev = doc.object();

        // Sidechain filter — subagent's own TodoWrite/TaskCreate/etc.
        // never count toward the parent's plan.
        if (ev.value(QStringLiteral("isSidechain")).toBool()) continue;

        // ANTS-1341 / ANTS-2115: advance `latestEventMs` AFTER the sidechain
        // filter but BEFORE the compact-summary skip. Sidechain events are a
        // subagent actively doing the parent's work — they are NOT parent
        // wall-clock progress, so letting them drive the abandonment clock
        // would treat a long-running subagent as if the parent task had gone
        // stale and silently drop a live `in_progress` task (ANTS-2115). A
        // `/compact` pause, by contrast, IS a genuine wall-clock gap in the
        // parent's own stream, so the compact-summary event (skipped below)
        // still counts — a task stuck before a compact must remain eligible
        // for the abandonment threshold.
        const qint64 evMs =
            parseIsoMs(ev.value(QStringLiteral("timestamp")).toString());
        if (evMs > latestEventMs) latestEventMs = evMs;

        // ANTS-1327 (2026-05-14, user-request): `isCompactSummary`
        // is NO LONGER a state-reset checkpoint. Previously (ANTS-1224,
        // 0.7.82) the parser cleared `out` on each compact event so
        // the chip showed only post-compact tasks; user feedback was
        // that the chip should mirror what Claude Code's own sidebar
        // shows — i.e. the *full* task history, including pre-compact
        // completed tasks. We now skip ONLY the compact-summary event
        // itself (it carries a synthetic conversation summary with no
        // tool_use blocks) and let prior pre-compact TaskCreate /
        // TodoWrite / TaskUpdate events keep contributing. The chip's
        // `done/total` numerator therefore reflects the lifetime of
        // the resumed session, matching the CC sidebar.
        if (ev.value(QStringLiteral("isCompactSummary")).toBool()) {
            continue;
        }

        const QString type = ev.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("assistant")) {
            const QJsonObject msg =
                ev.value(QStringLiteral("message")).toObject();
            const QJsonArray content =
                msg.value(QStringLiteral("content")).toArray();
            for (const QJsonValue &cv : content) {
                const QJsonObject c = cv.toObject();
                if (c.value(QStringLiteral("type")).toString()
                        != QLatin1String("tool_use"))
                    continue;
                const QString name =
                    c.value(QStringLiteral("name")).toString();
                const QString toolUseId =
                    c.value(QStringLiteral("id")).toString();
                const QJsonObject input =
                    c.value(QStringLiteral("input")).toObject();

                // Subagent-dispatch filter. Both `Task` and `Agent`
                // tool families share the `subagent_type` discriminant.
                if (input.contains(QStringLiteral("subagent_type")))
                    continue;

                if (name == QLatin1String("TodoWrite")) {
                    // Snapshot replaces the list.
                    out.clear();
                    idxByToolUseId.clear();
                    const QJsonArray todos =
                        input.value(QStringLiteral("todos")).toArray();
                    // ANTS-1407: an empty TodoWrite is a "no active todos"
                    // signal, not a mode commitment. Only lock out Mode B
                    // when CC actually listed items via TodoWrite; an empty
                    // list lets a subsequent TaskCreate resume Mode B.
                    sawTodoWrite = !todos.isEmpty();
                    for (const QJsonValue &tv : todos) {
                        ClaudeTask t = taskFromTodoEntry(tv.toObject());
                        // ANTS-1341: stamp every entry with the snapshot's
                        // event timestamp; TodoWrite is itself an event
                        // affecting every entry it lists.
                        t.lastEventAtMs = evMs;
                        out.append(std::move(t));
                    }
                    continue;
                }

                if (name == QLatin1String("TaskCreate")) {
                    if (sawTodoWrite) continue;  // Mode A wins
                    // ANTS-1246 batch-reset (2026-05-12 user report):
                    // a TaskCreate event that follows a fully-completed
                    // prior batch starts a fresh logical batch. Without
                    // this, completed work piles up forever in Mode B —
                    // user opens the Tasks dialog mid-session and sees
                    // 15 entries when the current logical batch is 4.
                    // TodoWrite (Mode A) already clears on snapshot;
                    // this brings TaskCreate to parity.
                    //
                    // ANTS-1407: widen the terminal predicate from
                    // `completed` only to `completed OR deleted`.
                    // Both are terminal in TaskUpdate semantics; a
                    // `deleted` task should not block a fresh burst.
                    if (!out.isEmpty()) {
                        bool allTerminal = true;
                        for (const auto &existing : out) {
                            if (existing.status
                                    != QLatin1String("completed")
                                && existing.status
                                    != QLatin1String("deleted")) {
                                allTerminal = false;
                                break;
                            }
                        }
                        if (allTerminal) {
                            out.clear();
                            idxByToolUseId.clear();
                        }
                    }
                    ClaudeTask t;
                    t.subject =
                        input.value(QStringLiteral("subject")).toString();
                    t.description =
                        input.value(QStringLiteral("description")).toString();
                    t.activeForm =
                        input.value(QStringLiteral("activeForm")).toString();
                    t.status = QStringLiteral("pending");
                    // ANTS-1341: stamp the create-time event timestamp.
                    t.lastEventAtMs = evMs;
                    out.append(t);
                    if (!toolUseId.isEmpty())
                        idxByToolUseId.insert(toolUseId, out.size() - 1);
                    continue;
                }

                if (name == QLatin1String("TaskUpdate")) {
                    if (sawTodoWrite) continue;  // Mode A wins
                    const QString taskId =
                        input.value(QStringLiteral("taskId")).toString();
                    if (taskId.isEmpty()) continue;
                    for (auto &t : out) {
                        if (t.id == taskId) {
                            const QString s =
                                input.value(QStringLiteral("status")).toString();
                            if (!s.isEmpty()) t.status = s;
                            const QString sub =
                                input.value(QStringLiteral("subject")).toString();
                            if (!sub.isEmpty()) t.subject = sub;
                            const QString desc =
                                input.value(QStringLiteral("description")).toString();
                            if (!desc.isEmpty()) t.description = desc;
                            // ANTS-1341: bump the touched task's last-event
                            // timestamp. This is what keeps an
                            // actively-updated `in_progress` task out of
                            // the abandonment filter.
                            t.lastEventAtMs = evMs;
                            break;
                        }
                    }
                    continue;
                }
            }
        } else if (type == QLatin1String("user") && !sawTodoWrite) {
            // Pair tool_result with the TaskCreate that preceded it
            // — id only available here.
            const QJsonObject msg =
                ev.value(QStringLiteral("message")).toObject();
            const QJsonArray content =
                msg.value(QStringLiteral("content")).toArray();
            for (const QJsonValue &cv : content) {
                const QJsonObject c = cv.toObject();
                if (c.value(QStringLiteral("type")).toString()
                        != QLatin1String("tool_result"))
                    continue;
                const QString tuId =
                    c.value(QStringLiteral("tool_use_id")).toString();
                if (tuId.isEmpty()) continue;
                auto it = idxByToolUseId.find(tuId);
                if (it == idxByToolUseId.end()) continue;
                const QString body =
                    ClaudeContent::toText(c.value(QStringLiteral("content")));
                const QString id = extractIdFromResultBody(body);
                if (!id.isEmpty()) out[it.value()].id = id;
            }
        }
    }

    // ANTS-1407: `deleted` tasks live in `out` during the walk so
    // that later TaskUpdate-by-id can find them, but the visible
    // tracker does not surface them — dialog header sums
    // (done + running + outstanding) already exclude `deleted`,
    // and the chip's total should match. Drop them as a final pass.
    out.erase(std::remove_if(out.begin(), out.end(),
        [](const ClaudeTask &t) {
            return t.status == QLatin1String("deleted");
        }),
        out.end());

    // ANTS-1341: abandonment filter. An `in_progress` task whose
    // last event is more than 24 h before the latest event in the
    // transcript is dropped — subagent crashed, partial network
    // failure, or transcript-replay artefact. A task with
    // lastEventAtMs == 0 (timestamp missing or unparseable) is
    // preserved (fail-soft: malformed timestamp must not drop
    // user-visible work). Reference time = transcript's latest
    // event, NOT wall-clock — deterministic against synthetic
    // fixtures.
    if (latestEventMs > 0) {
        const qint64 threshold = latestEventMs - kStaleInProgressMs;
        out.erase(std::remove_if(out.begin(), out.end(),
            [threshold](const ClaudeTask &t) {
                return t.status == QLatin1String("in_progress")
                       && t.lastEventAtMs > 0
                       && t.lastEventAtMs < threshold;
            }),
            out.end());
    }

    return out;
}
