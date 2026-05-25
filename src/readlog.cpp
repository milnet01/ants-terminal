// ANTS-1855 — read_log filtering helper. Pure (Qt6::Core-only). Streams
// the file line-by-line and keeps survivors in a drop-oldest deque
// bounded by max_bytes (and tail), so peak heap stays ≤ max_bytes
// regardless of file size. See docs/specs/ANTS-1855.md.

#include "readlog.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>

#include <climits>
#include <deque>

namespace ReadLog {

namespace {

// Serialised contribution of one line to the JSON lines[] array:
// the UTF-8 text + 2 quotes + 1 comma (escapes are rare in log text;
// a small under-count keeps the soft cap soft, per the spec).
int lineCost(const QByteArray &utf8) { return utf8.size() + 3; }

// The bracket-prefix value `since` compares against: the text between a
// leading '[' and the next ']'. Returns false when the line has no such
// prefix (those are dropped when `since` is set).
bool prefixGE(const QString &line, const QString &since) {
    if (!line.startsWith(QLatin1Char('['))) return false;
    const int close = line.indexOf(QLatin1Char(']'));
    if (close < 1) return false;
    return QStringView(line).mid(1, close - 1) >= since;
}

}  // namespace

QJsonObject filter(const QString &path, const Options &opts) {
    QJsonObject env;

    // Regex validation → bad_args (before opening the file).
    const bool haveInc = !opts.include.isEmpty();
    const bool haveExc = !opts.exclude.isEmpty();
    QRegularExpression incRe, excRe;
    if (haveInc) {
        incRe = QRegularExpression(opts.include);
        if (!incRe.isValid()) {
            env["ok"] = false; env["code"] = QStringLiteral("bad_args");
            env["error"] = QStringLiteral("read_log: invalid 'include' regex: %1")
                               .arg(incRe.errorString());
            return env;
        }
        incRe.optimize();
    }
    if (haveExc) {
        excRe = QRegularExpression(opts.exclude);
        if (!excRe.isValid()) {
            env["ok"] = false; env["code"] = QStringLiteral("bad_args");
            env["error"] = QStringLiteral("read_log: invalid 'exclude' regex: %1")
                               .arg(excRe.errorString());
            return env;
        }
        excRe.optimize();
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        env["ok"] = false; env["code"] = QStringLiteral("not_found");
        env["error"] = QStringLiteral("read_log: cannot open %1").arg(path);
        return env;
    }
    const qint64 fileSize = f.size();

    // Effective caps.
    int maxBytes = opts.maxBytes > 0 ? opts.maxBytes : kDefaultBytesCap;
    bool capClamped = false;
    if (maxBytes > kMaxBytesCeiling) { maxBytes = kMaxBytesCeiling; capClamped = true; }
    const int tailLimit = opts.tail > 0
                              ? (opts.tail > kMaxTail ? kMaxTail : opts.tail)
                              : INT_MAX;

    // since_cursor: soft-fallback on malformed / rotated (never a refusal).
    bool cursorStale = false;
    QString staleReason;
    qint64 startOffset = 0;
    if (opts.hasSinceCursor) {
        bool okNum = false;
        const qint64 cur = opts.sinceCursor.toLongLong(&okNum);
        if (!okNum || cur < 0) { cursorStale = true; staleReason = QStringLiteral("malformed_cursor"); }
        else if (cur > fileSize) { cursorStale = true; staleReason = QStringLiteral("rotated"); }
        else startOffset = cur;
    }
    if (startOffset > 0) f.seek(startOffset);

    const QString &contains = opts.contains;
    const bool haveContains = !contains.isEmpty();
    const bool haveSince = !opts.since.isEmpty();

    int scanned = 0;
    int matched = 0;
    std::deque<QByteArray> kept;     // raw UTF-8 (newline stripped)
    qint64 keptBytes = 0;
    qint64 cursorPos = startOffset;  // advances past each COMPLETE line

    while (!f.atEnd()) {
        const qint64 lineStart = f.pos();
        QByteArray raw = f.readLine();
        const bool complete = raw.endsWith('\n');
        if (!complete && f.atEnd()) {
            // Partial trailing line — hold it; cursor stops before it.
            cursorPos = lineStart;
            break;
        }
        cursorPos = f.pos();
        if (complete) raw.chop(1);   // strip the '\n'

        ++scanned;
        const QString line = QString::fromUtf8(raw);
        if (haveContains && !line.contains(contains)) continue;
        if (haveInc && !incRe.match(line).hasMatch()) continue;
        if (haveExc && excRe.match(line).hasMatch()) continue;
        if (haveSince && !prefixGE(line, opts.since)) continue;

        ++matched;
        kept.push_back(raw);
        keptBytes += lineCost(raw);
        // Drop-oldest until within both bounds (keep ≥1 line).
        while (static_cast<int>(kept.size()) > tailLimit ||
               (keptBytes > maxBytes && kept.size() > 1)) {
            keptBytes -= lineCost(kept.front());
            kept.pop_front();
        }
    }

    const int tailSelected = (opts.tail > 0)
                                 ? std::min(matched, tailLimit)
                                 : matched;
    const int returned = static_cast<int>(kept.size());
    const int linesDropped = tailSelected - returned;  // cap-removed (≥0)

    QJsonArray lines;
    for (const QByteArray &l : kept) lines.append(QString::fromUtf8(l));

    env["ok"] = true;
    env["path"] = path;
    env["lines"] = lines;
    env["matched"] = matched;
    env["scanned"] = scanned;
    env["returned"] = returned;
    env["truncated"] = linesDropped > 0;
    if (linesDropped > 0) env["lines_dropped"] = linesDropped;
    if (capClamped) env["bytes_cap_clamped"] = true;
    env["cursor"] = QString::number(cursorPos);
    if (cursorStale) {
        env["cursor_stale"] = true;
        env["stale_reason"] = staleReason;
    }
    return env;
}

}  // namespace ReadLog
