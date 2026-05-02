#include "claudetasklist.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

// Try to extract a task id from a tool_result body — Claude Code's
// confirmation message follows the shape "Task #N created
// successfully:" so we capture the digit run after `#`. Empty string
// when no match, in which case the entry's id stays empty (which is
// fine — id is only used for TaskUpdate matching, not for display).
QString extractIdFromResultBody(const QString &body) {
    static const QRegularExpression re(QStringLiteral(R"(Task\s+#(\d+))"));
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
    if (!m_transcriptPath.isEmpty() && QFileInfo::exists(m_transcriptPath))
        m_watcher.addPath(m_transcriptPath);
    rescan();
}

int ClaudeTaskListTracker::unfinishedCount() const {
    int n = 0;
    for (const auto &t : m_tasks) {
        if (t.status != QStringLiteral("completed")) ++n;
    }
    return n;
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

    while (!file.atEnd()) {
        const QByteArray rawLine = file.readLine().trimmed();
        if (rawLine.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(rawLine);
        if (!doc.isObject()) continue;
        const QJsonObject ev = doc.object();

        // Sidechain filter — subagent's own TodoWrite/TaskCreate/etc.
        // never count toward the parent's plan.
        if (ev.value(QStringLiteral("isSidechain")).toBool()) continue;

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
                    sawTodoWrite = true;
                    const QJsonArray todos =
                        input.value(QStringLiteral("todos")).toArray();
                    for (const QJsonValue &tv : todos) {
                        out.append(taskFromTodoEntry(tv.toObject()));
                    }
                    continue;
                }

                if (name == QLatin1String("TaskCreate")) {
                    if (sawTodoWrite) continue;  // Mode A wins
                    ClaudeTask t;
                    t.subject =
                        input.value(QStringLiteral("subject")).toString();
                    t.description =
                        input.value(QStringLiteral("description")).toString();
                    t.activeForm =
                        input.value(QStringLiteral("activeForm")).toString();
                    t.status = QStringLiteral("pending");
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
                    c.value(QStringLiteral("content")).toString();
                const QString id = extractIdFromResultBody(body);
                if (!id.isEmpty()) out[it.value()].id = id;
            }
        }
    }

    return out;
}
