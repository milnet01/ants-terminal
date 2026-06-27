// ANTS-2021 — read_region slicing helper. Pure (Qt6::Core-only). Reads the
// file line-by-line, stopping at the earlier of end_line or the incremental
// max_bytes boundary, so peak heap stays ≤ max_bytes regardless of the
// requested range or file size. Symbol mode resolves a [start,end) range via
// the flat file_outline. See docs/specs/ANTS-2021.md.

#include "readregion.h"

#include "fileoutline.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <climits>

namespace ReadRegion {

namespace {

// Serialised contribution of one line to the JSON lines[] array: UTF-8 text
// + 2 quotes + 1 comma (the same soft accounting capJsonArrayToBytes uses,
// computed incrementally — see ANTS-2021 § 2.5).
int lineCost(const QByteArray &utf8) { return utf8.size() + 3; }

// Resolve a symbol name to a 1-based inclusive [start, end] line range via
// the flat file_outline (no nesting level exists). end is the line before
// the next outline entry, or INT_MAX (to EOF) when the match is last.
struct SymRange {
    bool found      = false;
    int  start      = 0;
    int  end        = 0;
    int  matchCount = 0;
};

// ANTS-2222 — brace-balanced scan from an aggregate's declaration line to its
// matching closing brace. Returns the 1-based closing-brace line, or 0 if no
// balanced close is found (caller keeps the outline-derived fallback). Skips
// braces inside // and /* */ comments and "…"/'…' literals (heuristic, the
// same altitude as file_outline).
int aggregateEndLine(const QString &absPath, int startLine) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    int lineNo = 0, depth = 0;
    bool seenOpen = false, inBlockComment = false;
    while (!f.atEnd()) {
        const QByteArray raw = f.readLine();
        ++lineNo;
        if (lineNo < startLine) continue;
        const QString line = QString::fromUtf8(raw);
        bool inString = false, inChar = false;
        for (int i = 0; i < line.size(); ++i) {
            const QChar c = line.at(i);
            const QChar n = (i + 1 < line.size()) ? line.at(i + 1) : QChar();
            if (inBlockComment) {
                if (c == QLatin1Char('*') && n == QLatin1Char('/')) { inBlockComment = false; ++i; }
                continue;
            }
            if (inString) {
                if (c == QLatin1Char('\\')) ++i;
                else if (c == QLatin1Char('"')) inString = false;
                continue;
            }
            if (inChar) {
                if (c == QLatin1Char('\\')) ++i;
                else if (c == QLatin1Char('\'')) inChar = false;
                continue;
            }
            if (c == QLatin1Char('/') && n == QLatin1Char('/')) break;  // line comment
            if (c == QLatin1Char('/') && n == QLatin1Char('*')) { inBlockComment = true; ++i; continue; }
            if (c == QLatin1Char('"'))  { inString = true; continue; }
            if (c == QLatin1Char('\'')) { inChar = true; continue; }
            if (c == QLatin1Char('{')) { ++depth; seenOpen = true; }
            else if (c == QLatin1Char('}')) {
                if (--depth <= 0 && seenOpen) return lineNo;
            }
        }
        if (lineNo - startLine > 200000) break;  // defensive bound
    }
    return 0;
}

SymRange resolveSymbol(const QString &absPath, const QString &name) {
    SymRange r;
    // maxSymbols is hard-clamped to kMaxSymbolsCap (1000) inside compute;
    // pass a high value so resolution sees all of them up to that cap.
    const QJsonObject outline =
        FileOutline::compute(absPath, FileOutline::Mode::Auto,
                             /*includeDocComment=*/false, /*maxSymbols=*/1000);
    const QJsonArray syms = outline.value(QStringLiteral("symbols")).toArray();
    int firstIdx = -1;
    for (int i = 0; i < syms.size(); ++i) {
        if (syms.at(i).toObject().value(QStringLiteral("name")).toString() == name) {
            if (firstIdx < 0) firstIdx = i;
            ++r.matchCount;
        }
    }
    if (firstIdx < 0) return r;  // not found
    r.found = true;
    const QJsonObject symObj = syms.at(firstIdx).toObject();
    r.start = symObj.value(QStringLiteral("line")).toInt();
    const int outlineEnd = (firstIdx + 1 < syms.size())
        ? syms.at(firstIdx + 1).toObject().value(QStringLiteral("line")).toInt() - 1
        : INT_MAX;  // to EOF
    // ANTS-2222 — aggregate symbols (struct/class/union) get their FULL body.
    // The flat outline's "next entry" for a struct is its first member, so the
    // outline-derived end stops at the first field. Brace-match to the closing
    // brace instead. file_outline tags struct/class/namespace all as kind
    // "class"; the signature keyword distinguishes them — namespace is excluded
    // (its body can span the whole file, not a quotable unit).
    const QString kind = symObj.value(QStringLiteral("kind")).toString();
    const QString sig  = symObj.value(QStringLiteral("signature")).toString().trimmed();
    const bool isAggregate = kind == QLatin1String("class") &&
        (sig.startsWith(QLatin1String("struct")) ||
         sig.startsWith(QLatin1String("class"))  ||
         sig.startsWith(QLatin1String("union")));
    const int braceEnd = isAggregate ? aggregateEndLine(absPath, r.start) : 0;
    r.end = (braceEnd >= r.start) ? braceEnd : outlineEnd;
    if (r.end < r.start) r.end = r.start;
    return r;
}

}  // namespace

QJsonObject extract(const QString &absPath, const Options &opts) {
    QJsonObject env;

    // INV-3 — exactly one selector. hasLine == hasSym means neither or both.
    const bool hasSym = !opts.symbol.isEmpty();
    if (opts.hasLine == hasSym) {
        env["ok"] = false;
        env["code"] = QStringLiteral("bad_args");
        env["error"] = QStringLiteral(
            "read_region: exactly one of {line range, symbol} required");
        return env;
    }
    if (opts.hasLine && (opts.startLine < 1 || opts.endLine < opts.startLine)) {
        env["ok"] = false;
        env["code"] = QStringLiteral("bad_args");
        env["error"] = QStringLiteral(
            "read_region: start_line must be >= 1 and end_line >= start_line");
        return env;
    }

    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) {
        env["ok"] = false;
        env["code"] = QStringLiteral("not_found");
        env["error"] = QStringLiteral("read_region: cannot open %1").arg(absPath);
        return env;
    }

    int startLine = opts.startLine;
    int endLine   = opts.endLine;
    QString symbolEcho;
    bool symAmbiguous = false;
    int  symMatchCount = 0;
    if (hasSym) {
        const SymRange sr = resolveSymbol(absPath, opts.symbol);
        if (!sr.found) {
            env["ok"] = false;
            env["code"] = QStringLiteral("symbol_not_found");
            env["error"] =
                QStringLiteral("read_region: symbol not found: %1").arg(opts.symbol);
            return env;
        }
        startLine = sr.start;
        endLine = sr.end;
        symbolEcho = opts.symbol;
        symMatchCount = sr.matchCount;
        symAmbiguous = sr.matchCount > 1;
        // A symbol resolver that returned an out-of-shape range is treated
        // as a defensive bad_args rather than reading garbage.
        if (startLine < 1) startLine = 1;
    }

    int maxBytes = opts.maxBytes > 0 ? opts.maxBytes : kDefaultBytesCap;
    bool capClamped = false;
    if (maxBytes > kMaxBytesCeiling) { maxBytes = kMaxBytesCeiling; capClamped = true; }

    QJsonArray lines;
    qint64 keptBytes = 0;
    int lineNo = 0;
    int effectiveEnd = startLine - 1;  // none emitted yet
    bool truncated = false;

    while (!f.atEnd()) {
        QByteArray raw = f.readLine();
        if (raw.endsWith('\n')) raw.chop(1);
        ++lineNo;
        if (lineNo < startLine) continue;
        if (lineNo > endLine) break;

        const int cost = lineCost(raw);
        // Incremental cap: keep ≥1 line, then stop before overflowing.
        if (!lines.isEmpty() && keptBytes + cost > maxBytes) {
            truncated = true;
            break;  // this line and all later ones are never read (INV-9)
        }
        lines.append(QString::fromUtf8(raw));
        keptBytes += cost;
        effectiveEnd = lineNo;
    }

    const int returned = lines.size();
    env["ok"] = true;
    env["path"] = absPath;
    env["start_line"] = startLine;
    // When nothing was emitted (start past EOF, or an empty range), echo the
    // pre-start sentinel so the caller sees end_line < start_line.
    env["end_line"] = returned > 0 ? effectiveEnd : startLine - 1;
    env["lines"] = lines;
    env["returned"] = returned;
    env["truncated"] = truncated;
    if (capClamped) env["bytes_cap_clamped"] = true;
    if (hasSym) {
        env["symbol"] = symbolEcho;
        if (symAmbiguous) {
            env["symbol_ambiguous"] = true;
            env["symbol_match_count"] = symMatchCount;
        }
    }

    // ANTS-2157 — integration brief: the ordered call-expressions inside the
    // returned region (the pipeline's STAGES, with line anchors = insertion
    // points) + the accessors a new stage typically needs (m_ members +
    // get/is/has getters referenced). Answers "to add a step here, what are
    // the existing steps in order and the helpers they use?" in one call.
    // A heuristic line scan (same Karpathy-§2 bet as file_outline); reuses
    // the region this call already sliced. The first kept line in symbol
    // mode is the signature — its own call is not a stage.
    if (opts.callSequence) {
        static const QRegularExpression callRx(
            QStringLiteral(R"((?:^|[^\w.>])([A-Za-z_]\w*)\s*\()"));
        static const QRegularExpression memRx(QStringLiteral(R"(\bm_[A-Za-z_]\w*)"));
        static const QRegularExpression getRx(
            QStringLiteral(R"(\b((?:get|is|has)[A-Za-z_]\w*)\s*\()"));
        static const QSet<QString> kw = {
            QStringLiteral("if"), QStringLiteral("for"), QStringLiteral("while"),
            QStringLiteral("switch"), QStringLiteral("return"), QStringLiteral("sizeof"),
            QStringLiteral("catch"), QStringLiteral("do"), QStringLiteral("else"),
            QStringLiteral("new"), QStringLiteral("delete"), QStringLiteral("throw"),
            QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("not"),
            QStringLiteral("co_await"), QStringLiteral("co_return"),
        };
        constexpr int kMaxSeq = 300;
        QJsonArray seq;
        QSet<QString> accessors;
        for (int i = 0; i < lines.size(); ++i) {
            const QString ln = lines.at(i).toString();
            const int lineNum = startLine + i;
            const bool isSignatureLine = (hasSym && i == 0);
            if (!isSignatureLine && seq.size() < kMaxSeq) {
                auto it = callRx.globalMatch(ln);
                while (it.hasNext() && seq.size() < kMaxSeq) {
                    const QString callee = it.next().captured(1);
                    if (kw.contains(callee)) continue;
                    QJsonObject c;
                    c[QStringLiteral("line")]   = lineNum;
                    c[QStringLiteral("callee")] = callee;
                    seq.append(c);
                }
            }
            for (auto mit = memRx.globalMatch(ln); mit.hasNext(); )
                accessors.insert(mit.next().captured(0));
            for (auto git = getRx.globalMatch(ln); git.hasNext(); )
                accessors.insert(git.next().captured(1));
        }
        QStringList accList(accessors.begin(), accessors.end());
        std::sort(accList.begin(), accList.end());
        env["call_sequence"] = seq;
        env["call_sequence_truncated"] = (seq.size() >= kMaxSeq);
        env["accessors"] = QJsonArray::fromStringList(accList);
    }
    return env;
}

}  // namespace ReadRegion
