#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <utility>

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
// ANTS-5050 — a resumable position in one transcript.
//
// `offset` is the byte position up to which COMPLETE lines have been
// consumed; `latestEventMs` is the clock `walk` would have returned for
// everything consumed so far; `primed` distinguishes "nothing read yet" from
// "read, and legitimately at offset 0" (an empty file).
//
// Held by the trackers across rescans alongside their accumulated parse
// state. The two must be reset together — a cursor without its accumulator
// resumes into a half-built list.
struct Cursor {
    qint64 offset = 0;
    qint64 latestEventMs = 0;
    bool   primed = false;
};

// ANTS-5050 INV-4 — may `cur` be resumed against the file at `path`?
//
// False on an unprimed cursor, on a file shorter than the offset (truncated,
// or replaced by something smaller), and when the byte before the offset is
// not a newline (rewritten in place rather than appended to). A false answer
// obliges the caller to clear its accumulator before walking, because the
// walk will then start from the beginning.
//
// This is the staleness test ANTS-1500's `since_cursor` uses for the
// scrollback ring, applied to a file offset: when the cursor cannot be
// trusted, fall back to the full read rather than guessing. That is what
// keeps truncation, replacement and rotation a single branch.
inline bool canResume(const QString &path, const Cursor &cur) {
    if (!cur.primed || cur.offset <= 0) return false;
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) return false;
    if (file.size() < cur.offset) return false;
    if (!file.seek(cur.offset - 1)) return false;
    char prev = 0;
    return file.read(&prev, 1) == 1 && prev == '\n';
}

// Walk the transcript from `cur`, invoking `handler(ev, evMs)` per surviving
// event and advancing `cur` over what it consumed. The gating above applies
// unchanged — this is the one implementation of it.
//
// Two rules the full walk did not need (ANTS-5050):
//
//   • INV-1 — only COMPLETE lines are consumed. Claude appends to the
//     transcript continuously, so a read can land mid-line; the cursor stops
//     at that line's first byte and the line is parsed once it is complete.
//     Advancing past it would drop it permanently.
//   • INV-5 — the 16 MiB tail cap is a COLD-walk rule. A resumed walk starts
//     at the cursor and reads to EOF, so an append is never skipped for
//     sitting below the cap.
//
// A caller resuming MUST have checked `canResume` and, on false, cleared both
// the cursor and its own accumulator: this function walks from the beginning
// whenever the cursor is not primed, and a stale offset it was handed anyway
// would resume into the wrong file.
template <typename Handler>
void walkFrom(const QString &path, Cursor &cur, Handler &&handler) {
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) return;

    const bool resuming = cur.primed && cur.offset > 0;
    if (resuming) {
        if (!file.seek(cur.offset)) return;
    } else {
        constexpr qint64 kMaxBytes = 16LL * 1024 * 1024;
        const qint64 size = file.size();
        if (size > kMaxBytes) {
            if (!file.seek(size - kMaxBytes)) return;
            file.readLine();  // discard the (likely-truncated) first line
        }
        cur.latestEventMs = 0;
        cur.offset = file.pos();
    }

    while (!file.atEnd()) {
        const qint64 lineStart = file.pos();
        const QByteArray rawLine = file.readLine();
        // INV-1 — a line with no terminator is still being written. Leave the
        // cursor before it and stop; the next walk sees it whole.
        if (!rawLine.endsWith('\n')) {
            cur.offset = lineStart;
            break;
        }
        cur.offset = file.pos();

        const QByteArray trimmed = rawLine.trimmed();
        if (trimmed.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed);
        if (!doc.isObject()) continue;
        const QJsonObject ev = doc.object();

        if (ev.value(QStringLiteral("isSidechain")).toBool()) continue;

        const qint64 evMs =
            parseIsoMs(ev.value(QStringLiteral("timestamp")).toString());
        if (evMs > cur.latestEventMs) cur.latestEventMs = evMs;

        if (ev.value(QStringLiteral("isCompactSummary")).toBool()) continue;

        handler(ev, evMs);
    }
    cur.primed = true;
}

// The full walk, unchanged in contract: walk the whole file and return the
// latest-event clock. Expressed over `walkFrom` with a fresh cursor so the
// gating and the tail cap have exactly one implementation — ANTS-1261 exists
// because two copies of this prologue drifted, and adding an incremental
// variant beside it would have recreated that.
template <typename Handler>
qint64 walk(const QString &path, Handler &&handler) {
    Cursor cur;
    walkFrom(path, cur, std::forward<Handler>(handler));
    return cur.latestEventMs;
}

}  // namespace ClaudeTranscript
