// ANTS-2021 — read_region slicing helper. Pure (Qt6::Core-only). Reads the
// file line-by-line, stopping at the earlier of end_line or the incremental
// max_bytes boundary, so peak heap stays ≤ max_bytes regardless of the
// requested range or file size. Symbol mode resolves a [start,end) range via
// the flat file_outline. See docs/specs/ANTS-2021.md.

#include "readregion.h"

#include "fileoutline.h"
#include "pathvalidation.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

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

// ANTS-2222 / ANTS-2224 — brace-balanced scan from a symbol's declaration line
// to its matching closing brace. Returns the 1-based closing-brace line, or 0
// if no balanced close is found (caller keeps the outline-derived fallback).
// Serves both aggregate bodies (struct/class/union) and function bodies. Skips
// braces inside // and /* */ comments and "…"/'…' literals (heuristic, the
// same altitude as file_outline).
int braceBalancedEndLine(const QString &absPath, int startLine) {
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
    int defIdx   = -1;  // ANTS-3349 — first match that is a definition
    for (int i = 0; i < syms.size(); ++i) {
        const QJsonObject so = syms.at(i).toObject();
        if (so.value(QStringLiteral("name")).toString() == name) {
            if (firstIdx < 0) firstIdx = i;
            ++r.matchCount;
            // Prefer the definition over a forward declaration: a declaration
            // signature ends with ';' (`void Foo();`); a definition's does not
            // (`void Foo() {` / `void Foo()`). Mirrors find_definition's kind.
            if (defIdx < 0) {
                const QString s =
                    so.value(QStringLiteral("signature")).toString().trimmed();
                if (!s.endsWith(QLatin1Char(';'))) defIdx = i;
            }
        }
    }
    if (firstIdx < 0) return r;  // not found
    r.found = true;
    const int chosenIdx = (defIdx >= 0) ? defIdx : firstIdx;
    const QJsonObject symObj = syms.at(chosenIdx).toObject();
    r.start = symObj.value(QStringLiteral("line")).toInt();
    const int outlineEnd = (chosenIdx + 1 < syms.size())
        ? syms.at(chosenIdx + 1).toObject().value(QStringLiteral("line")).toInt() - 1
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
    if (isAggregate) {
        // The flat outline's "next entry" for a struct is its first member, so
        // the outline-derived end stops there. Brace-match to the real close.
        const int braceEnd = braceBalancedEndLine(absPath, r.start);
        r.end = (braceEnd >= r.start) ? braceEnd : outlineEnd;
    } else if (kind == QLatin1String("func")) {
        // ANTS-2224 — belt-and-braces cap on function bodies. When file_outline
        // misses the next symbol (e.g. an extern "C" fn — the ANTS-2159 gap),
        // outlineEnd extends past the real closing brace and swallows the
        // following definition (DOOM repro: a fn closing at 2387 returned
        // end 2470). Cap at the balanced closing '}'. std::min only ever
        // TIGHTENS: a normal body's brace-end equals outlineEnd; a body-less
        // declaration brace-matches past the next symbol so min keeps
        // outlineEnd; the last symbol's INT_MAX (to-EOF) collapses to the close.
        const int braceEnd = braceBalancedEndLine(absPath, r.start);
        r.end = (braceEnd >= r.start) ? std::min(braceEnd, outlineEnd) : outlineEnd;
    } else {
        r.end = outlineEnd;
    }
    if (r.end < r.start) r.end = r.start;
    return r;
}

// ANTS-2221 — GitHub-style heading slug: lowercase, every run of
// non-alphanumeric characters collapses to a single '-', and leading/
// trailing '-' are trimmed. Idempotent (the slug of a slug is itself), so a
// caller may pass either the heading text ("4.2 Emission model") or its slug
// ("4-2-emission-model") and both resolve to the same key.
QString mdHeadingSlug(const QString &text) {
    QString out;
    out.reserve(text.size());
    bool pendingDash = false;
    for (const QChar c : text) {
        if (c.isLetterOrNumber()) {
            if (pendingDash && !out.isEmpty()) out.append(QLatin1Char('-'));
            pendingDash = false;
            out.append(c.toLower());
        } else {
            pendingDash = true;
        }
    }
    return out;
}

// ANTS-2221 — resolve a markdown heading slug to its section-body range: the
// ATX-heading line through the line BEFORE the next heading of the same or a
// higher level (≤ '#' count), or to EOF when none follows. Tracks ``` / ~~~
// fenced code so a '#' inside a code block is never read as a heading. First
// slug match wins; failing that, a dash-bounded prefix match resolves a
// heading's short title when it carries a trailing parenthetical (ANTS-2234),
// but only when exactly one heading qualifies. The markdown analogue of
// resolveSymbol for code.
struct SecRange {
    bool        found = false;
    int         start = 0;   // 1-based heading line
    int         end   = 0;   // 1-based last line (INT_MAX → to EOF)
    QString     slug;        // the RESOLVED heading slug (may differ from the query)
    bool        ambiguous = false;  // ANTS-2234 — >1 prefix candidate
    QStringList candidates;         // the qualifying heading slugs when ambiguous
};

int mdHeadingLevel(const QString &trimmed) {
    int h = 0;
    while (h < trimmed.size() && trimmed.at(h) == QLatin1Char('#')) ++h;
    // ATX heading: 1-6 leading '#' then a space (or the whole line).
    if (h >= 1 && h <= 6 &&
        (h == trimmed.size() || trimmed.at(h) == QLatin1Char(' ')))
        return h;
    return 0;
}

SecRange resolveSection(const QString &absPath, const QString &wantSlug) {
    SecRange r;
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return r;

    // One pass: collect every (fence-aware) ATX heading as {line, level, slug}.
    struct Head { int line{}; int level{}; QString slug; };
    QVector<Head> heads;
    int lineNo = 0;
    bool inFence = false;
    QString fenceMarker;
    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine());
        ++lineNo;
        if (line.endsWith(QLatin1Char('\n'))) line.chop(1);
        const QString trimmed = line.trimmed();
        // Fence toggling: ``` or ~~~ opens, the same marker family closes.
        if (trimmed.startsWith(QLatin1String("```")) ||
            trimmed.startsWith(QLatin1String("~~~"))) {
            const QString marker = trimmed.left(3);
            if (!inFence) { inFence = true; fenceMarker = marker; }
            else if (marker == fenceMarker) { inFence = false; }
            continue;
        }
        if (inFence) continue;
        const int level = mdHeadingLevel(trimmed);
        if (level == 0) continue;
        heads.push_back({lineNo, level, mdHeadingSlug(trimmed.mid(level).trimmed())});
    }

    // Tier 1 — exact slug (back-compat: full heading text / full slug). First
    // match wins. Tier 2 (ANTS-2234) — when no exact match, a dash-bounded
    // prefix (`<wantSlug>-…`) resolves a short title whose heading carries a
    // trailing parenthetical, but ONLY when exactly one heading qualifies;
    // ≥2 is ambiguous and refused rather than guessed.
    int idx = -1;
    for (int i = 0; i < heads.size(); ++i)
        if (heads[i].slug == wantSlug) { idx = i; break; }
    if (idx < 0 && !wantSlug.isEmpty()) {
        const QString pfx = wantSlug + QLatin1Char('-');
        QVector<int> cands;
        for (int i = 0; i < heads.size(); ++i)
            if (heads[i].slug.startsWith(pfx)) cands.push_back(i);
        if (cands.size() == 1) {
            idx = cands.front();
        } else if (cands.size() > 1) {
            r.ambiguous = true;
            for (int i : cands) r.candidates << heads[i].slug;
            return r;
        }
    }
    if (idx < 0) return r;  // not found

    r.found = true;
    r.start = heads[idx].line;
    r.slug  = heads[idx].slug;  // the resolved heading's slug
    const int matchLevel = heads[idx].level;
    // End = line before the next heading at the same-or-higher level, else EOF.
    r.end = INT_MAX;
    for (int i = idx + 1; i < heads.size(); ++i)
        if (heads[i].level <= matchLevel) { r.end = heads[i].line - 1; break; }
    return r;
}

}  // namespace

QJsonObject extract(const QString &absPath, const Options &opts) {
    QJsonObject env;

    // INV-3 — exactly one selector of {line range, symbol, section}.
    const bool hasSym = !opts.symbol.isEmpty();
    const bool hasSec = !opts.section.isEmpty();
    const int selectorCount =
        (opts.hasLine ? 1 : 0) + (hasSym ? 1 : 0) + (hasSec ? 1 : 0);
    if (selectorCount != 1) {
        env["ok"] = false;
        env["code"] = QStringLiteral("bad_args");
        env["error"] = QStringLiteral(
            "read_region: exactly one of {line range, symbol, section} required");
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
    QString sectionEcho, sectionSlugEcho;
    bool symAmbiguous = false;
    int  symMatchCount = 0;
    if (hasSec) {
        const QString wantSlug = mdHeadingSlug(opts.section);
        const SecRange sec = resolveSection(absPath, wantSlug);
        if (sec.ambiguous) {
            // ANTS-2234 — a short title that prefixes ≥2 headings is refused,
            // not guessed; the candidate slugs tell the agent which to pick.
            env["ok"] = false;
            env["code"] = QStringLiteral("section_ambiguous");
            env["error"] = QStringLiteral(
                "read_region: section: %1 matches %2 headings — pass a fuller "
                "title or the slug")
                    .arg(opts.section).arg(sec.candidates.size());
            env["candidates"] = QJsonArray::fromStringList(sec.candidates);
            return env;
        }
        if (!sec.found) {
            env["ok"] = false;
            env["code"] = QStringLiteral("section_not_found");
            env["error"] = QStringLiteral(
                "read_region: no markdown heading matches section: %1")
                    .arg(opts.section);
            return env;
        }
        startLine       = sec.start;
        endLine         = sec.end;
        sectionEcho     = opts.section;
        sectionSlugEcho = sec.slug;  // resolved heading slug (may differ from input)
    }
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
    if (hasSec) {
        env["section"] = sectionEcho;
        env["section_slug"] = sectionSlugEcho;
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

namespace {

// ANTS-2219 — hex16-of-sha256 of a result object's compact JSON. Same shape
// as the file_outline multi-path per-file etag, so callers see one etag
// format across the MCP read surface.
QString sliceEtag(const QJsonObject &obj) {
    const QByteArray buf = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(QCryptographicHash::hash(
        buf, QCryptographicHash::Sha256).toHex().left(16));
}

// ANTS-2219 — resolve ONE batch item (path + selector) under rootCanonical:
// validate the path through the central PathValidation chokepoint, run
// extract() with the supplied byte budget, and reframe the echoed path
// project-relative. Returns the slice envelope on success, or an ok:false
// {code,error} object so the batch loop records one bad item without aborting.
QJsonObject readOneRegion(const QJsonObject &item,
                          const QString &rootCanonical, int maxBytes) {
    const QString rawPath = item.value(QStringLiteral("path")).toString();
    if (rawPath.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_regions: item missing \"path\"");
        o["code"]  = QStringLiteral("bad_args");
        return o;
    }
    const PathValidation::Check check = PathValidation::validatePath(
        rawPath, rootCanonical,
        QStringLiteral("read_regions"), QStringLiteral("path"));
    if (check.bad) return check.err;
    if (check.resolved.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_regions: \"%1\" does not exist")
                         .arg(rawPath);
        o["code"]  = QStringLiteral("not_found");
        return o;
    }
    Options opts;
    opts.symbol   = item.value(QStringLiteral("symbol")).toString();
    opts.section  = item.value(QStringLiteral("section")).toString();
    opts.maxBytes = maxBytes;
    const QJsonValue startV = item.value(QStringLiteral("start_line"));
    const QJsonValue endV   = item.value(QStringLiteral("end_line"));
    if (startV.isDouble()) {
        opts.hasLine   = true;
        opts.startLine = startV.toInt();
        opts.endLine   = endV.isDouble() ? endV.toInt() : opts.startLine;
    }
    QJsonObject result = extract(check.resolved, opts);
    // Reframe the echoed absolute path to project-relative for stable,
    // launch-location-independent paths.
    if (result.value(QStringLiteral("ok")).toBool()) {
        const QString abs = result.value(QStringLiteral("path")).toString();
        if (abs.startsWith(rootCanonical + QLatin1Char('/')))
            result["path"] = abs.mid(rootCanonical.size() + 1);
    }
    return result;
}

}  // namespace

QJsonObject extractBatch(const QString &rootCanonical,
                         const QJsonValue &itemsValue, int maxBytes) {
    if (!itemsValue.isArray()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_regions: \"items\" array is required");
        o["code"]  = QStringLiteral("bad_args");
        return o;
    }
    const QJsonArray items = itemsValue.toArray();
    if (items.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("read_regions: \"items\" must not be empty");
        o["code"]  = QStringLiteral("bad_args");
        return o;
    }
    constexpr int kMaxItems = 64;
    if (items.size() > kMaxItems) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral(
            "read_regions: too many items (%1 > %2) — split the batch")
                .arg(items.size()).arg(kMaxItems);
        o["code"]  = QStringLiteral("too_many_items");
        return o;
    }

    // Shared byte budget across the set: default 512 KiB, clamped to the
    // 4 MiB read-family ceiling. Consumed in item order; once exhausted, a
    // later item gets a 1-byte floor (extract keeps >= 1 line and flags
    // truncated) rather than silently re-expanding to the per-call default.
    int budget = maxBytes > 0 ? maxBytes : kDefaultBytesCap;
    if (budget > kMaxBytesCeiling) budget = kMaxBytesCeiling;

    QJsonArray results;
    bool anyTruncated = false, budgetExhausted = false;
    for (const QJsonValue &iv : items) {
        const QJsonObject item = iv.toObject();
        const int itemCap = budget > 0 ? budget : 1;
        QJsonObject slice = readOneRegion(item, rootCanonical, itemCap);
        const QString etag = sliceEtag(slice);
        const QString prior =
            item.value(QStringLiteral("etag_match")).toString();
        QJsonObject entry;
        if (slice.value(QStringLiteral("ok")).toBool() && !prior.isEmpty() &&
            prior == etag) {
            entry["path"]      = slice.value(QStringLiteral("path"));
            entry["ok"]        = true;
            entry["unchanged"] = true;
            entry["etag"]      = etag;
        } else {
            slice["etag"] = etag;
            if (slice.value(QStringLiteral("truncated")).toBool())
                anyTruncated = true;
            entry = slice;
        }
        budget -= QJsonDocument(entry).toJson(QJsonDocument::Compact).size();
        if (budget <= 0) budgetExhausted = true;
        results.append(entry);
    }

    QJsonObject out;
    out["ok"]      = true;
    out["results"] = results;
    out["count"]   = results.size();
    if (anyTruncated || budgetExhausted) out["truncated"] = true;
    return out;
}

}  // namespace ReadRegion
