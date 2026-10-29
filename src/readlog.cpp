// ANTS-1855 — read_log filtering helper. Pure (Qt6::Core-only). Streams
// the file line-by-line and keeps survivors in a drop-oldest deque
// bounded by max_bytes (and tail), and reads no line past max_bytes, so
// peak heap stays near max_bytes regardless of file size. See docs/specs/ANTS-1855.md.

#include "readlog.h"
#include "secretredact.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

#include <climits>
#include <deque>
#include <vector>

namespace ReadLog {

namespace {

// Serialised contribution of one line to the JSON lines[] array: the
// UTF-8 text + 2 quotes + 1 comma + its JSON escapes (ANTS-5110 — a line
// of quotes or control bytes serialises at up to six times its size).
int lineCost(const QByteArray &utf8) {
    int cost = utf8.size() + 3;
    for (const char ch : utf8) {
        const auto u = static_cast<unsigned char>(ch);
        if (u == '"' || u == '\\' || u == '\b' || u == '\f' || u == '\n'
            || u == '\r' || u == '\t') {
            cost += 1;                 // \" \\ \b \f \n \r \t
        } else if (u < 0x20) {
            cost += 5;                 // \u00XX
        }
    }
    return cost;
}

// The bracket-prefix value `since` compares against: the text between a
// leading '[' and the next ']'. Returns false when the line has no such
// prefix (those are dropped when `since` is set).
bool prefixGE(const QString &line, const QString &since) {
    if (!line.startsWith(QLatin1Char('['))) return false;
    const int close = line.indexOf(QLatin1Char(']'));
    if (close < 1) return false;
    return QStringView(line).mid(1, close - 1) >= since;
}

// ANTS-5707 — where a line sits against private-key blocks, which span
// lines that SecretRedact::scrub sees one at a time (spec § 2.6 step 6).
enum class Pem { Outside, Inside, HeadPending };

// One survivor: its raw text and where it sits against key blocks.
struct Kept {
    QByteArray raw;
    Pem pem;
};

const QRegularExpression &pemBegin() {
    static const QRegularExpression re(
        QStringLiteral("-----BEGIN [^\\-\\n]*PRIVATE KEY-----"));
    return re;
}
const QRegularExpression &pemEnd() {
    static const QRegularExpression re(
        QStringLiteral("-----END [^\\-\\n]*PRIVATE KEY-----"));
    return re;
}

// Key-block state across every scanned line, kept or not. A slice
// starting past byte 0 may start inside a block: its lines stay
// HeadPending until an END marker (inside) or a BEGIN marker (outside)
// settles them, as SecretRedact::scrubCutPemHead does for one string.
// Settling rewrites the pem of lines already in `kept`.
class PemTracker {
public:
    explicit PemTracker(bool headPending) : m_headPending(headPending) {}

    // Where `line` sits; advances the block state past it.
    Pem classify(const QString &line, std::deque<Kept> &kept) {
        if (!line.contains(QLatin1String("PRIVATE KEY-----"))) {
            if (m_inBlock) return Pem::Inside;
            return m_headPending ? Pem::HeadPending : Pem::Outside;
        }
        const QRegularExpressionMatch b = pemBegin().match(line);
        const QRegularExpressionMatch e = pemEnd().match(line);
        const int bAt = b.hasMatch() ? b.capturedStart() : -1;
        const int eAt = e.hasMatch() ? e.capturedStart() : -1;
        if (m_headPending) {
            if (eAt >= 0 && (bAt < 0 || eAt < bAt)) {
                settle(kept, Pem::Inside);   // the slice began inside a block
                m_inBlock = bAt > eAt;       // a new block opens after it
                return Pem::Inside;
            }
            if (bAt < 0) return Pem::HeadPending;
            settle(kept, Pem::Outside);
        }
        if (m_inBlock) {
            if (eAt >= 0)
                m_inBlock = pemBegin().match(line, e.capturedEnd()).hasMatch();
            return Pem::Inside;
        }
        if (bAt < 0) return Pem::Outside;
        m_inBlock = !pemEnd().match(line, b.capturedEnd()).hasMatch();
        return Pem::Inside;
    }

    // At the end of the scan: a head never settled was not inside a block.
    void finish(std::deque<Kept> &kept) {
        if (m_headPending) settle(kept, Pem::Outside);
    }

private:
    void settle(std::deque<Kept> &kept, Pem to) {
        m_headPending = false;
        for (Kept &k : kept)
            if (k.pem == Pem::HeadPending) k.pem = to;
    }

    bool m_inBlock = false;
    bool m_headPending;
};

// The key-block markers of a line read in pieces, joined by spaces, so
// an oversized line is classified without being held whole. A marker
// split across two pieces is found in the tail carried from the first;
// one longer than that tail is missed.
class PieceMarkers {
public:
    void feed(const QByteArray &piece) {
        static const QRegularExpression kMarker(
            QStringLiteral("-----(BEGIN|END) [^\\-\\n]*PRIVATE KEY-----"));
        const QString window = QString::fromLatin1(m_carry + piece);
        auto it = kMarker.globalMatch(window);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (m.capturedEnd() > m_carry.size())   // not found last piece
                m_markers << m.captured();
        }
        m_carry = (m_carry + piece).right(kCarry);
    }
    QString line() const { return m_markers.join(QLatin1Char(' ')); }

private:
    static constexpr int kCarry = 128;
    QByteArray m_carry;
    QStringList m_markers;
};

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
    std::deque<Kept> kept;           // raw UTF-8 (newline stripped)
    qint64 keptBytes = 0;
    qint64 cursorPos = startOffset;  // advances past each COMPLETE line
    int oversizeSkipped = 0;         // lines longer than maxBytes (B-INV-11)

    // ANTS-5110 — a wall-clock budget, so a huge log cannot hold the worker.
    // Checked every 256 lines, between lines, so `cursor` resumes the scan.
    const qint64 budgetMs = opts.timeBudgetMs > 0 ? opts.timeBudgetMs
                                                  : kDefaultTimeBudgetMs;
    QElapsedTimer clock;
    clock.start();
    bool budgetExhausted = false;

    // ANTS-5707 — key-block tracking over every scanned line, kept or not.
    PemTracker pems(opts.redactSecrets && startOffset > 0);

    while (!f.atEnd()) {
        if (scanned > 0 && (scanned & 255) == 0 && clock.elapsed() > budgetMs) {
            budgetExhausted = true;
            break;
        }
        const qint64 lineStart = f.pos();
        // Never hold more than maxBytes of one line: a longer line is
        // drained in maxBytes pieces and skipped whole, its key-block
        // markers still tracked.
        QByteArray raw = f.readLine(qint64(maxBytes) + 1);
        if (!raw.endsWith('\n') && !f.atEnd()) {
            PieceMarkers markers;
            markers.feed(raw);
            QByteArray piece;
            do {
                piece = f.readLine(qint64(maxBytes) + 1);
                markers.feed(piece);
            } while (!piece.endsWith('\n') && !f.atEnd());
            if (!piece.endsWith('\n')) {   // oversized AND still being written
                cursorPos = lineStart;
                break;
            }
            if (opts.redactSecrets) pems.classify(markers.line(), kept);
            cursorPos = f.pos();
            ++scanned;
            ++oversizeSkipped;
            continue;
        }
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
        const Pem pem = opts.redactSecrets ? pems.classify(line, kept)
                                           : Pem::Outside;
        if (haveContains && !line.contains(contains)) continue;
        if (haveInc && !incRe.match(line).hasMatch()) continue;
        if (haveExc && excRe.match(line).hasMatch()) continue;
        if (haveSince && !prefixGE(line, opts.since)) continue;

        ++matched;
        keptBytes += lineCost(raw);
        kept.push_back({std::move(raw), pem});
        // Drop-oldest until within both bounds (keep ≥1 line).
        while (static_cast<int>(kept.size()) > tailLimit ||
               (keptBytes > maxBytes && kept.size() > 1)) {
            keptBytes -= lineCost(kept.front().raw);
            kept.pop_front();
        }
    }

    pems.finish(kept);

    // ANTS-5707 — scrub the kept lines, then re-apply the byte cap: a
    // redaction label can be longer than the secret it replaces.
    struct Out { QString text; int redacted; int cost; };
    std::vector<Out> out;
    out.reserve(kept.size());
    qint64 outBytes = 0;
    for (const Kept &k : kept) {
        Out o{QString::fromUtf8(k.raw), 0, 0};
        if (k.pem == Pem::Inside) {
            o.text = QStringLiteral("[REDACTED:private_key]");
            o.redacted = 1;
        } else if (opts.redactSecrets) {
            SecretRedact::Result r = SecretRedact::scrub(o.text);
            o.text = std::move(r.text);
            o.redacted = r.redactedCount;
        }
        o.cost = lineCost(o.text.toUtf8());
        outBytes += o.cost;
        out.push_back(std::move(o));
    }
    std::size_t first = 0;
    while (outBytes > maxBytes && out.size() - first > 1)
        outBytes -= out[first++].cost;

    const int tailSelected = (opts.tail > 0)
                                 ? std::min(matched, tailLimit)
                                 : matched;
    const int returned = static_cast<int>(out.size() - first);
    const int linesDropped = tailSelected - returned;  // cap-removed (≥0)

    QJsonArray lines;
    int redacted = 0;
    for (std::size_t i = first; i < out.size(); ++i) {
        lines.append(out[i].text);
        redacted += out[i].redacted;
    }

    env["ok"] = true;
    env["path"] = path;
    env["lines"] = lines;
    env["matched"] = matched;
    env["scanned"] = scanned;
    env["returned"] = returned;
    env["truncated"] = linesDropped > 0;
    if (linesDropped > 0) env["lines_dropped"] = linesDropped;
    if (oversizeSkipped > 0) {
        env["truncated"] = true;
        env["lines_oversize_skipped"] = oversizeSkipped;
    }
    if (budgetExhausted) {
        env["truncated"] = true;
        env["time_budget_exhausted"] = true;
    }
    if (capClamped) env["bytes_cap_clamped"] = true;
    if (redacted > 0) env["redacted"] = redacted;
    env["cursor"] = QString::number(cursorPos);
    if (cursorStale) {
        env["cursor_stale"] = true;
        env["stale_reason"] = staleReason;
    }
    return env;
}

}  // namespace ReadLog
