#include "claudetasklist.h"
#include "claudecontent.h"   // ANTS-2002 — content-as-array text extraction
#include "claudetranscriptwalker.h"  // ANTS-1261 — shared JSONL walk + gating

#include <algorithm>

#include <QHash>
#include <QJsonArray>
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

// ANTS-1341: abandonment threshold. An `in_progress` task whose
// last event is more than 24 h before the latest event in the
// transcript is dropped as abandoned (subagent crashed, partial
// network failure, transcript-replay artefact). Other statuses
// (pending = unstarted backlog, completed = history) are NOT
// time-bounded.
constexpr qint64 kStaleInProgressMs = 24LL * 3600LL * 1000LL;

} // namespace

ClaudeTaskListTracker::ClaudeTaskListTracker(QObject *parent)
    : ClaudeTranscriptTrackerOf(parent) {}

QList<ClaudeTask> ClaudeTaskListTracker::parse(const QString &path,
                                               ClaudeTranscript::Cursor &cursor,
                                               ClaudeTaskAccum &acc) const {
    return parseIncremental(path, cursor, acc);
}

bool ClaudeTaskListTracker::sameRow(const ClaudeTask &a, const ClaudeTask &b) const {
    return a.id == b.id && a.status == b.status && a.subject == b.subject;
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

// Pure parser. Walks the transcript line by line. See header for
// event shapes; full disambiguation table at docs/specs/ANTS-1158-claude-code-task-list-status.md
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
namespace {

// ANTS-5050 — fold one event into the accumulator. Extracted verbatim from
// parseTranscript's walk handler so the full and incremental paths share one
// implementation; nothing about the per-event semantics changed.
void applyTaskEvent(ClaudeTaskAccum &acc, const QJsonObject &ev,
                           qint64 evMs) {
    QList<ClaudeTask> &out = acc.raw;
    bool &sawTodoWrite = acc.sawTodoWrite;
    QHash<QString, int> &idxByToolUseId = acc.idxByToolUseId;

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

// ANTS-5050 — the end-of-parse filters. Pure and re-runnable over the
// accumulator, which is what lets a resumed walk produce the same answer as a
// cold one: `acc.raw` keeps the `deleted` entries a later TaskUpdate needs,
// and both filters are applied to a copy on every emit.
QList<ClaudeTask> finalizeTasks(const ClaudeTaskAccum &acc,
                                       qint64 latestEventMs) {
    QList<ClaudeTask> out = acc.raw;

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

}  // namespace

QList<ClaudeTask> ClaudeTaskListTracker::parseIncremental(
        const QString &path, ClaudeTranscript::Cursor &cursor,
        ClaudeTaskAccum &acc) {
    ClaudeTranscript::walkFrom(path, cursor,
        [&acc](const QJsonObject &ev, qint64 evMs) {
            applyTaskEvent(acc, ev, evMs);
        });
    return finalizeTasks(acc, cursor.latestEventMs);
}

QList<ClaudeTask> ClaudeTaskListTracker::parseTranscript(const QString &path) {
    // The full parse is the incremental one from a fresh cursor. Keeping it
    // expressed that way is what makes ANTS-5050 INV-8 hold by construction
    // rather than by a second copy of the same walk.
    ClaudeTranscript::Cursor cursor;
    ClaudeTaskAccum acc;
    return parseIncremental(path, cursor, acc);
}
