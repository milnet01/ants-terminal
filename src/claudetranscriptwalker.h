#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// ANTS-1261 — shared Claude Code transcript walker.
//
// `ClaudeTaskListTracker::parseTranscript` (TodoWrite / TaskCreate plans)
// and `ClaudeBgTaskTracker::parseTranscript` (run_in_background launches)
// parse *different* event payloads, but both walked the JSONL transcript
// with byte-identical scaffolding — the 16 MiB tail cap, the
// blank-line / non-object skip, and the isSidechain / isCompactSummary
// gating — and that shared prologue had begun to drift between the two
// copies. This factors the walk + gating into one place; each tracker
// plugs in its own per-event handler.
//
// Pure (Qt6::Core only) so the two tracker consumers and their feature
// tests share one implementation, mirroring claudecontent.h (ANTS-2002).
namespace ClaudeTranscript {

// ANTS-1341 — parse an ISO-8601-with-ms timestamp string into epoch ms.
// Returns 0 on empty / unparseable input — fail-soft so a missing or
// malformed timestamp never drops user-visible work from the abandonment
// filter.
inline qint64 parseIsoMs(const QString &ts) {
    if (ts.isEmpty()) return 0;
    const QDateTime dt = QDateTime::fromString(ts, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

// Walk the Claude Code JSONL transcript at `path`, invoking
// `handler(ev, evMs)` once per surviving event in document order. `ev` is
// the parsed event object; `evMs` is that event's own timestamp (epoch
// ms, 0 if absent). Returns the latest timestamp seen across all
// non-sidechain events (0 if none parseable) — the task-list tracker uses
// it as the deterministic reference time for its abandonment threshold;
// the bg-task tracker ignores it.
//
// Gating applied before the handler fires, in this exact order (the order
// is load-bearing — see ANTS-2115 below):
//   • 16 MiB tail cap. On an over-cap file the head is dropped and the
//     first (likely-truncated) line discarded; only old already-finished
//     entries are lost, which is acceptable for both trackers.
//   • blank lines and non-object JSON lines skipped.
//   • isSidechain events skipped entirely — a subagent's inline turns
//     never count toward the parent session, and (ANTS-2115) they do NOT
//     advance the returned latest-event clock: a long-running subagent
//     must not look like parent wall-clock progress.
//   • the latest-event clock then advances over the event — INCLUDING an
//     isCompactSummary event. A `/compact` pause is a genuine wall-clock
//     gap in the parent's own stream, so a task stranded just before a
//     compact must stay eligible for the abandonment threshold
//     (ANTS-2115 / ANTS-1327).
//   • isCompactSummary events are then skipped (they carry a synthetic
//     conversation summary with no tool_use payload) — so the handler
//     never sees them, but their timestamp has already been counted.
//
// `evMs` is parsed for every surviving event even when the handler does
// not consult it (the bg tracker). The parse is a cheap fail-soft
// QDateTime::fromString and the trackers run on debounced file changes,
// so owning the clock here — rather than duplicating the timestamp logic
// in each handler — keeps the ANTS-2115 ordering in one tested place.
template <typename Handler>
qint64 walk(const QString &path, Handler &&handler) {
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) return 0;

    constexpr qint64 kMaxBytes = 16 * 1024 * 1024;
    const qint64 size = file.size();
    if (size > kMaxBytes) {
        if (!file.seek(size - kMaxBytes)) return 0;
        file.readLine();  // discard the (likely-truncated) first line
    }

    qint64 latestEventMs = 0;
    while (!file.atEnd()) {
        const QByteArray rawLine = file.readLine().trimmed();
        if (rawLine.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(rawLine);
        if (!doc.isObject()) continue;
        const QJsonObject ev = doc.object();

        if (ev.value(QStringLiteral("isSidechain")).toBool()) continue;

        const qint64 evMs =
            parseIsoMs(ev.value(QStringLiteral("timestamp")).toString());
        if (evMs > latestEventMs) latestEventMs = evMs;

        if (ev.value(QStringLiteral("isCompactSummary")).toBool()) continue;

        handler(ev, evMs);
    }
    return latestEventMs;
}

}  // namespace ClaudeTranscript
