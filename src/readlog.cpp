// ANTS-1855 — read_log filtering helper. Pure (Qt6::Core-only). Streams
// the file line-by-line and keeps survivors in a drop-oldest deque
// bounded by max_bytes (and tail), and reads no line past max_bytes, so
// peak heap stays near max_bytes regardless of file size. See docs/specs/ANTS-1855-read-log-mcp-tool-filter.md.

#include "readlog.h"
#include "boundedreadline.h"
#include "regexharden.h"
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

// ANTS-6632 — a debug-log line of the `input` or `pty` category:
// `[timestamp] <category padded to 8>  <message>` (DebugLog::write).
bool isInputPtyLine(const QString &line) {
    if (!line.startsWith(QLatin1Char('['))) return false;
    const int close = line.indexOf(QLatin1String("] "));
    if (close < 1) return false;
    const QStringView rest = QStringView(line).mid(close + 2);
    const auto isCategory = [&rest](QLatin1String name) {
        return rest.startsWith(name)
            && (rest.size() == name.size() || rest.at(name.size()) == QLatin1Char(' '));
    };
    return isCategory(QLatin1String("input")) || isCategory(QLatin1String("pty"));
}

// ANTS-5707 — where a line sits against private-key blocks, which span
// lines that SecretRedact::scrub sees one at a time (spec § 2.6 step 6).
enum class Pem { Outside, Inside, HeadPending };

// One survivor: its raw text and where it sits against key blocks.
struct Kept {
    QByteArray raw;
    Pem pem;
};

// An OpenSSH or PEM key, or (with " BLOCK") an armoured PGP private key.
const QRegularExpression &pemBegin() {
    static const QRegularExpression re(
        QStringLiteral("-----BEGIN [^\\-\\n]*PRIVATE KEY(?: BLOCK)?-----"));
    return re;
}
const QRegularExpression &pemEnd() {
    static const QRegularExpression re(
        QStringLiteral("-----END [^\\-\\n]*PRIVATE KEY(?: BLOCK)?-----"));
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
        if (!line.contains(QLatin1String("PRIVATE KEY"))) {
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
            QStringLiteral("-----(BEGIN|END) [^\\-\\n]*PRIVATE KEY(?: BLOCK)?-----"));
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

// The next line of the file, as one of three outcomes.
struct NextLine {
    enum Kind {
        Line,        // a complete line: `raw`, newline stripped
        Oversized,   // longer than one read: drained and skipped
        Unfinished,  // the file ends before its newline: still being written
    } kind;
    QByteArray raw;
    QString markers;  // Oversized: its key-block markers, space-joined
};

// Reads one line, holding at most `readSize` - 1 bytes of it. A longer
// line is drained in pieces and skipped whole, its key-block markers
// still collected. Every read goes through `scratch` (ANTS-6367).
NextLine readNextLine(QIODevice &f, qint64 readSize, QByteArray &scratch) {
    QByteArray raw = BoundedReadLine::readTight(f, readSize, scratch);
    if (raw.endsWith('\n')) {
        raw.chop(1);
        return {NextLine::Line, std::move(raw), {}};
    }
    if (f.atEnd()) return {NextLine::Unfinished, {}, {}};
    PieceMarkers markers;
    markers.feed(raw);
    QByteArray piece;
    do {
        piece = BoundedReadLine::readTight(f, readSize, scratch);
        markers.feed(piece);
    } while (!piece.endsWith('\n') && !f.atEnd());
    if (!piece.endsWith('\n')) return {NextLine::Unfinished, {}, {}};
    return {NextLine::Oversized, {}, markers.line()};
}

// The lines[] array to return, and how many secrets it had redacted.
struct Scrubbed {
    QJsonArray lines;
    int redacted = 0;
};

// ANTS-5707 — scrub the kept lines, then re-apply the byte cap: a
// redaction label can be longer than the secret it replaces. Drops the
// oldest lines until within maxBytes, keeping at least one.
Scrubbed scrubAndRecap(const std::deque<Kept> &kept, bool redactSecrets,
                       int maxBytes) {
    struct Out { QString text; int redacted; int cost; };
    std::vector<Out> out;
    out.reserve(kept.size());
    qint64 outBytes = 0;
    for (const Kept &k : kept) {
        Out o{QString::fromUtf8(k.raw), 0, 0};
        if (k.pem == Pem::Inside) {
            o.text = QStringLiteral("[REDACTED:private_key]");
            o.redacted = 1;
        } else if (redactSecrets) {
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

    Scrubbed s;
    for (std::size_t i = first; i < out.size(); ++i) {
        s.lines.append(out[i].text);
        s.redacted += out[i].redacted;
    }
    return s;
}

}  // namespace

QJsonObject filter(const QString &path, const Options &opts) {
    QJsonObject env;

    // Regex validation → bad_args (before opening the file). ANTS-6988 — a
    // pattern is capped in length and runs under PCRE2's match limit, so one
    // line's match cannot hold the worker: a line whose match hits the limit
    // counts as not matching.
    const bool haveInc = !opts.include.isEmpty();
    const bool haveExc = !opts.exclude.isEmpty();
    QRegularExpression incRe, excRe;
    const auto compile = [&env](const QString &pattern, const char *name,
                                QRegularExpression &re) {
        QString why;
        if (pattern.size() > kMaxPatternChars) {
            why = QStringLiteral("longer than %1 characters").arg(kMaxPatternChars);
        } else {
            re = QRegularExpression(ants::regex::hardenUserRegex(pattern));
            if (re.isValid()) {
                re.optimize();
                return true;
            }
            why = re.errorString();
        }
        env["ok"] = false; env["code"] = QStringLiteral("bad_args");
        env["error"] = QStringLiteral("read_log: invalid '%1' regex: %2")
                           .arg(QLatin1String(name), why);
        return false;
    };
    if (haveInc && !compile(opts.include, "include", incRe)) return env;
    if (haveExc && !compile(opts.exclude, "exclude", excRe)) return env;

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
    int inputPtyOmitted = 0;         // ANTS-6632 — input/pty lines left out

    // ANTS-5110 — a wall-clock budget, so a huge log cannot hold the worker.
    // Checked between lines, so `cursor` resumes the scan; after every line,
    // as one slow include/exclude match can take most of the budget. An
    // oversized line is drained whole first, so it can overrun the budget
    // (ANTS-1855 INV-13).
    const qint64 budgetMs = opts.timeBudgetMs > 0 ? opts.timeBudgetMs
                                                  : kDefaultTimeBudgetMs;
    QElapsedTimer clock;
    clock.start();
    bool budgetExhausted = false;

    // ANTS-5707 — key-block tracking over every scanned line, kept or not.
    PemTracker pems(opts.redactSecrets && startOffset > 0);

    // Every read goes through one buffer, so a kept line holds only its
    // own bytes and not a maxBytes-sized allocation (ANTS-6367).
    QByteArray scratch;
    // readTight returns at most its size - 1 bytes, so + 2 keeps a line of
    // exactly maxBytes with its newline.
    const qint64 readSize = qint64(maxBytes) + 2;
    while (!f.atEnd()) {
        if (scanned > 0 && clock.elapsed() > budgetMs) {
            budgetExhausted = true;
            break;
        }
        const qint64 lineStart = f.pos();
        NextLine next = readNextLine(f, readSize, scratch);
        if (next.kind == NextLine::Unfinished) {
            // Hold a line still being written; the cursor stops before it.
            cursorPos = lineStart;
            break;
        }
        cursorPos = f.pos();
        ++scanned;
        if (next.kind == NextLine::Oversized) {
            if (opts.redactSecrets) pems.classify(next.markers, kept);
            ++oversizeSkipped;
            continue;
        }

        QByteArray raw = std::move(next.raw);
        const QString line = QString::fromUtf8(raw);
        const Pem pem = opts.redactSecrets ? pems.classify(line, kept)
                                           : Pem::Outside;
        if (opts.omitInputPty && isInputPtyLine(line)) { ++inputPtyOmitted; continue; }
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
    const Scrubbed scrubbed = scrubAndRecap(kept, opts.redactSecrets, maxBytes);

    const int tailSelected = (opts.tail > 0)
                                 ? std::min(matched, tailLimit)
                                 : matched;
    const int returned = static_cast<int>(scrubbed.lines.size());
    const int linesDropped = tailSelected - returned;  // cap-removed (≥0)

    env["ok"] = true;
    env["path"] = path;
    env["lines"] = scrubbed.lines;
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
    if (scrubbed.redacted > 0) env["redacted"] = scrubbed.redacted;
    if (inputPtyOmitted > 0) env["input_pty_omitted"] = inputPtyOmitted;
    env["cursor"] = QString::number(cursorPos);
    if (cursorStale) {
        env["cursor_stale"] = true;
        env["stale_reason"] = staleReason;
    }
    return env;
}

}  // namespace ReadLog
