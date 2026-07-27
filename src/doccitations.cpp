// ANTS-3653 — see doccitations.h for the contract, docs/specs/ANTS-3653.md for
// the grammar.

#include "doccitations.h"

#include "markdownscan.h"
#include "pathvalidation.h"

#include <QChar>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QLatin1Char>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace DocCitations {

namespace {

constexpr char16_t kEnDash = 0x2013;

// seg := [A-Za-z0-9_.@+-]. ASCII by construction — QChar::isLetterOrNumber()
// would admit Unicode letters and widen the left edge past what the grammar
// says.
bool isSegChar(QChar c) {
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z')
           || (u >= u'0' && u <= u'9')
           || u == u'_' || u == u'.' || u == u'@' || u == u'+' || u == u'-';
}

bool isDigit(QChar c) {
    const char16_t u = c.unicode();
    return u >= u'0' && u <= u'9';
}

// path := "/"? seg ( "/" seg )*, and the LAST seg must contain a ".". The dot
// stands in for "names a file", and a directory component's dot says nothing
// about the leaf: `src.old/README:12` does not parse. Every character is
// already a seg char or "/" (the left-edge walk admits nothing else), so only
// the shape is checked here.
bool validPath(const QString &p) {
    if (p.isEmpty()) return false;
    const QString body = p.startsWith(QLatin1Char('/')) ? p.mid(1) : p;
    if (body.isEmpty()) return false;
    const QStringList segs = body.split(QLatin1Char('/'));
    for (const QString &s : segs)
        if (s.isEmpty()) return false;               // no "//", no trailing "/"
    return segs.last().contains(QLatin1Char('.'));
}

// One `"~"? D` locus. D is GREEDY — the whole digit run — so stage 2 can see
// how long it was and report it; a `[0-9]{1,cap}(?![0-9])` production would
// match nothing and the token would vanish, reported by no field at all.
struct Locus {
    bool tilde  = false;
    int  start  = 0;   // index of the first digit
    int  len    = 0;   // digit-run length
};

// Parse a locus at `k`; on success advance `k` past it.
bool parseLocus(const QString &s, int &k, Locus &out) {
    int i = k;
    Locus l;
    if (i < s.size() && s.at(i) == QLatin1Char('~')) { l.tilde = true; ++i; }
    l.start = i;
    while (i < s.size() && isDigit(s.at(i))) ++i;
    l.len = i - l.start;
    if (l.len == 0) return false;
    out = l;
    k   = i;
    return true;
}

// A token recognised by stage 1: the boundaries plus the kept loci.
struct Token {
    int   end   = 0;         // one past the token's last character
    Locus first;
    bool  hasSecond = false;
    Locus second;
    bool  partial   = false;
};

// stage 1 — `":" rawlocus trailing?` at `colon`. False when what follows the
// colon is not a locus at all (`foo.cpp:bar`), in which case there is no token
// and nothing to report.
bool recogniseAfterColon(const QString &s, int colon, Token &tok) {
    int k = colon + 1;
    Token t;
    if (!parseLocus(s, k, t.first)) return false;

    // rawlocus := "~"? D [ ("-" | "–") "~"? D ] — the corpus writes both
    // separators, and a hyphen-only rule loses a large minority of the ranges.
    // A separator with no locus after it is not part of the token.
    if (k < s.size()
        && (s.at(k) == QLatin1Char('-') || s.at(k).unicode() == kEnDash)) {
        int probe = k + 1;
        Locus second;
        if (parseLocus(s, probe, second)) {
            t.hasSecond = true;
            t.second    = second;
            k           = probe;
        }
    }

    // trailing := ( "/" "~"? D )+ | "+" — EXHAUSTIVE, and the two forms do not
    // combine. Everything else ends the token, so the "." closing a sentence in
    // `see src/a.cpp:12.` is ordinary punctuation and sets no flag.
    if (k < s.size() && s.at(k) == QLatin1Char('+')) {
        t.partial = true;
        ++k;
    } else {
        while (k < s.size() && s.at(k) == QLatin1Char('/')) {
            int probe = k + 1;
            Locus dropped;
            if (!parseLocus(s, probe, dropped)) break;
            t.partial = true;
            k         = probe;
        }
    }
    t.end = k;
    tok   = t;
    return true;
}

// stage 2 — a recognised token is a citation iff every locus it KEPT satisfies
// len(D) <= maxLocusDigits and then value(D) >= 1, and, for a range,
// start <= end. The LENGTH test runs first, so no over-large digit run is ever
// converted to an int. A `~` on a dropped trailing locus is dropped with it,
// which is why only the kept loci are read here.
bool validate(const QString &s, const Token &t, const Options &opts,
              Citation &out) {
    if (t.first.len > opts.maxLocusDigits) return false;
    if (t.hasSecond && t.second.len > opts.maxLocusDigits) return false;

    const int start = s.mid(t.first.start, t.first.len).toInt();
    const int end   = t.hasSecond ? s.mid(t.second.start, t.second.len).toInt()
                                  : start;
    if (start < 1 || end < 1 || start > end) return false;

    out.startLine   = start;
    out.endLine     = end;
    out.approximate = t.first.tilde || (t.hasSecond && t.second.tilde);
    out.partial     = t.partial;
    return true;
}

// CommonMark § 6.1's one-space strip. The consumer's job, not the primitive's:
// a caller matching an identifier is unaffected by it, while "fills the span"
// applies it — `` ` :45 ` `` is what a renderer shows the author as `:45`.
// Returns the number of characters removed from the FRONT so the caller can
// keep reporting real columns.
int oneSpaceStrip(const QString &content, QString &stripped) {
    stripped = content;
    if (content.size() >= 2 && content.startsWith(QLatin1Char(' '))
        && content.endsWith(QLatin1Char(' '))
        && !content.trimmed().isEmpty()) {
        stripped = content.mid(1, content.size() - 2);
        return 1;
    }
    return 0;
}

void record(ScanResult &r, const Options &opts, const QString &line, int docLine,
          int docCol, const Token &tok, const QString &raw,
          const QString &path, bool continuation) {
    Citation c;
    c.docLine      = docLine;
    c.docCol       = docCol;
    c.raw          = raw;
    c.path         = path;
    c.continuation = continuation;
    if (validate(line, tok, opts, c)) {
        r.citations.append(c);
        return;
    }
    Unparsed u;
    u.docLine    = docLine;
    u.docCol     = docCol;
    u.raw        = raw.left(opts.maxRawChars);
    u.rawClipped = raw.size() > opts.maxRawChars;
    u.reason     = QStringLiteral("bad_locus");
    r.unparsed.append(u);
}

}  // namespace

ScanResult scan(const QStringList &lines, const Options &opts) {
    ScanResult r;
    const QVector<bool> fence = MarkdownScan::fenceMask(lines,
                                                        &r.unterminatedFence);

    // Pass 1 — path-bearing tokens, on the RAW line. A citation with a path is
    // harvested wherever it appears: bare prose, inside emphasis, inside link
    // text, or inside an inline code span (where doc_integrity masks spans out,
    // this verb reads them — the policy inverts, the scanner is shared).
    for (int li = 0; li < lines.size(); ++li) {
        if (fence.value(li)) continue;               // fenced ⇒ illustrative
        const QString &line = lines.at(li);
        for (int i = 0; i < line.size(); ++i) {
            if (line.at(i) != QLatin1Char(':')) continue;

            // Maximal munch LEFTWARD from the colon: walk back over seg
            // characters and "/", stopping at the first character that is
            // neither. In "word-src/a.cpp:12" the token is the whole
            // word-src/a.cpp:12 — which resolves nowhere — never the
            // src/a.cpp:12 inside it. Without this rule both `raw` and the
            // document-order column key are undefined for such a token.
            int start = i;
            while (start > 0
                   && (isSegChar(line.at(start - 1))
                       || line.at(start - 1) == QLatin1Char('/')))
                --start;
            const QString path = line.mid(start, i - start);
            if (!validPath(path)) continue;

            Token tok;
            if (!recogniseAfterColon(line, i, tok)) continue;
            record(r, opts, line, li + 1, start, tok,
                 line.mid(start, tok.end - start), path, false);
            i = tok.end - 1;                          // resume past the token
        }
    }

    // Pass 2 — continuations. A bare `:N` counts ONLY when it fills a whole
    // inline code span: `:45` inside `09:45`, or `:1` inside the ratio `3:1`,
    // is coincidence, and harvesting it would inherit an unrelated path and
    // emit that file's line 45 under status "ok". The delimiters are what make
    // a continuation authored rather than coincidental. A span containing a
    // newline never qualifies.
    for (const MarkdownScan::CodeSpan &s : MarkdownScan::codeSpans(lines, fence)) {
        if (s.startLine != s.endLine) continue;
        const QString &line = lines.at(s.startLine);
        QString stripped;
        const int off = s.startCol
                        + oneSpaceStrip(line.mid(s.startCol,
                                                 s.endCol - s.startCol),
                                        stripped);
        if (!stripped.startsWith(QLatin1Char(':'))) continue;

        Token tok;
        if (!recogniseAfterColon(stripped, 0, tok)) continue;
        if (tok.end != stripped.size()) continue;     // did not FILL the span
        record(r, opts, stripped, s.startLine + 1, off, tok, stripped, QString(),
             true);
    }

    // Document order — ascending docLine, then ascending column. Paging over
    // this is only meaningful against a defined order, and two citations on one
    // line are two entries that must not swap between runs.
    const auto byPosition = [](const auto &a, const auto &b) {
        return a.docLine != b.docLine ? a.docLine < b.docLine
                                      : a.docCol < b.docCol;
    };
    std::stable_sort(r.citations.begin(), r.citations.end(), byPosition);
    std::stable_sort(r.unparsed.begin(), r.unparsed.end(), byPosition);
    return r;
}

}  // namespace DocCitations

// ---------------------------------------------------------------------------
// ANTS-3636 — the read path: resolve every scanned citation against the tree
// and return the cited text. See docs/specs/ANTS-3636.md § 2.4 (the ladder),
// § 2.6 (statuses) and § 2.7 (the Options block that owns every bound).
// ---------------------------------------------------------------------------

namespace DocCitations {

namespace {

constexpr int kChunkBytes = 64 * 1024;

// UTF-8 decode with the two conditions § 2.6 calls read_error. A QTextStream
// would silently substitute U+FFFD, which is exactly the mojibake text the spec
// forbids, so the decoder's error flag is tested rather than trusted — and the
// raw bytes are scanned for a NUL, which decodes cleanly but is not text.
bool decodeChecked(const QByteArray &bytes, QString *out) {
    if (bytes.contains('\0')) return false;
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString s = dec(bytes);
    if (dec.hasError()) return false;
    if (out) *out = s;
    return true;
}

// Split into lines without inventing a trailing empty one, and drop the CR of a
// CRLF file (INV-23). An empty file is zero lines, which is what makes a
// citation to its line 1 out_of_range with no special case.
QStringList splitLines(const QString &text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.constLast().isEmpty()) lines.removeLast();
    for (QString &l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    return lines;
}

// Clip to `maxBytes` UTF-8 bytes, cutting between characters. The unit is bytes
// because every budget this feeds is in bytes; a character cap would let one
// CJK or emoji line cost 3-4x its budgeted size. The length is computed
// arithmetically per code point rather than by re-encoding prefixes, so a
// multi-megabyte line costs one pass and no allocations.
QString clipToBytes(const QString &line, int maxBytes, bool *clipped) {
    if (line.size() <= maxBytes / 4) return line;      // cannot possibly exceed
    int used = 0;
    int k    = 0;
    const int n = line.size();
    while (k < n) {
        const QChar c = line.at(k);
        char32_t cp   = c.unicode();
        int step      = 1;
        if (c.isHighSurrogate() && k + 1 < n && line.at(k + 1).isLowSurrogate()) {
            cp   = QChar::surrogateToUcs4(c, line.at(k + 1));
            step = 2;
        }
        const int add = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (used + add > maxBytes) {
            if (clipped) *clipped = true;
            return line.left(k);
        }
        used += add;
        k    += step;
    }
    return line;
}

// The tokens wrapMcpData (src/claudeintegration.cpp) rewrites on the way out.
// The rewrite is correct and must not be bypassed — it is what stops a hostile
// file line from closing the wrap — so the citation discloses instead.
bool needsEscaping(const QString &line) {
    static const QRegularExpression closeRe(QStringLiteral(R"(</\s*ants_mcp_data\s*>)"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression openRe(QStringLiteral(R"(<\s*ants_mcp_data\b[^>]*>)"),
                                           QRegularExpression::CaseInsensitiveOption);
    return line.contains(QLatin1String("<!--")) || line.contains(QLatin1String("-->"))
           || line.contains(closeRe) || line.contains(openRe);
}

// Reads each target at most once while it is cached; past the cache's bounds a
// target is re-read per citation (no eviction, no LRU churn). ReadRegion::extract
// is deliberately not reused: it re-opens per call, never reports the file's
// line count, and its truncation is byte-based where range_truncated is
// line-based.
class TargetReader {
public:
    struct Result {
        bool        ok        = false;   // false → read_error
        int         fileLines = 0;
        QStringList lines;               // the emitted slice, already clipped
        bool        clipped   = false;
    };

    explicit TargetReader(const Options &o) : m_opts(o) {}

    Result read(const QString &absPath, int startLine, int wantLines) {
        Result r;
        const auto cached = m_cache.constFind(absPath);
        if (cached != m_cache.constEnd()) {
            r.ok        = true;
            r.fileLines = cached->size();
            for (int i = startLine - 1; i >= 0 && i < cached->size() && r.lines.size() < wantLines;
                 ++i)
                r.lines << clipToBytes(cached->at(i), m_opts.maxTextBytes, &r.clipped);
            return r;
        }
        if (m_reads >= m_opts.maxTargetReads) {   // the absolute budget, INV-46
            m_budgetHit = true;
            return r;
        }
        if (QFileInfo(absPath).size() > m_opts.maxTargetBytes) return r;
        QFile f(absPath);
        ++m_reads;
        if (!f.open(QIODevice::ReadOnly)) return r;
        if (m_opts.probe) ++m_opts.probe->opens;

        // Stream: a readAll() + split() would hold the bytes, the decoded string
        // and the list at once. A target that will be cached accumulates inside
        // the cache's own budget; one that will not retains only the lines the
        // response will emit, so a :1-9999999 citation costs the same as a :1-3.
        QStringList accum;
        bool   accumulating = m_cache.size() < m_opts.maxCacheFiles;
        qint64 entryCharge  = 0;
        int    lineNo       = 0;
        bool   bad          = false;
        QString pending;
        QStringDecoder dec(QStringDecoder::Utf8);

        const auto takeLine = [&](QString line) {
            if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
            ++lineNo;
            if (lineNo >= startLine && r.lines.size() < wantLines)
                r.lines << clipToBytes(line, m_opts.maxTextBytes, &r.clipped);
            if (!accumulating) return;
            // In memory, not on disk: QString is UTF-16, plus a header and its
            // own allocation per line. Budgeting by file size would promise
            // 8 MiB and deliver ~24.
            entryCharge += 2 * line.size() + 40;
            if (m_charge + entryCharge > m_opts.maxCacheBytes) {
                accumulating = false;      // abandoned — r.lines already has what it needs
                accum.clear();
                accum.squeeze();
                return;
            }
            accum << line;
        };

        while (!f.atEnd()) {
            const QByteArray chunk = f.read(kChunkBytes);
            if (chunk.contains('\0')) { bad = true; break; }
            pending += dec(chunk);
            if (dec.hasError()) { bad = true; break; }
            int nl;
            while ((nl = pending.indexOf(QLatin1Char('\n'))) >= 0) {
                takeLine(pending.left(nl));
                pending.remove(0, nl + 1);
            }
        }
        if (bad) return r;
        if (!pending.isEmpty()) takeLine(pending);

        r.ok        = true;
        r.fileLines = lineNo;
        if (accumulating) {
            m_cache.insert(absPath, accum);
            m_charge += entryCharge;
        }
        return r;
    }

    bool budgetHit() const { return m_budgetHit; }

private:
    const Options              &m_opts;
    QHash<QString, QStringList> m_cache;
    qint64                      m_charge    = 0;
    int                         m_reads     = 0;
    bool                        m_budgetHit = false;
};

// One citation's target, after the § 2.4 ladder.
struct Target {
    enum Kind { Resolved, MissingFile, Ambiguous, Unresolved, OutOfRoot, Unresolvable };
    Kind        kind = Unresolvable;
    QString     relPath;
    QString     absPath;
    QStringList candidates;
};

// Step 0's second branch: does the path climb above its own base? Purely
// lexical — no filesystem call, which is what INV-6 asserts.
bool lexicallyEscapes(const QString &p) {
    int depth = 0;
    for (const QString &seg : p.split(QLatin1Char('/'))) {
        if (seg == QLatin1String("..")) {
            if (--depth < 0) return true;
        } else if (!seg.isEmpty() && seg != QLatin1String(".")) {
            ++depth;
        }
    }
    return false;
}

// The regular-file test, then gate G. Both belong to the READ step, not to
// resolution, which is why a continuation inheriting a path still runs them.
// The stat is a regular-file test rather than an existence test: the grammar
// only requires a dot in the final segment, which a directory can satisfy.
Target statAndGate(const QString &root, const QString &relPath, const Options &opts) {
    Target t;
    t.relPath = relPath;
    const QString abs = root + QLatin1Char('/') + relPath;
    if (opts.probe) ++opts.probe->stats;
    const QFileInfo fi(abs);
    if (!fi.exists() || !fi.isFile()) {
        t.kind = Target::MissingFile;
        return t;
    }
    // Gate G — canonicalise BOTH sides. A QFileInfo::isSymLink check would
    // inspect only the final component, so src/link/foo.cpp where src/link
    // points outside the root would pass it.
    if (opts.probe) ++opts.probe->stats;
    if (!PathValidation::isInsideProject(root, abs)) {
        t.kind = Target::OutOfRoot;
        return t;
    }
    t.kind    = Target::Resolved;
    t.absPath = abs;
    return t;
}

Target resolvePath(const QString &root, const QString &pathText, const Options &opts) {
    Target t;
    if (pathText.startsWith(QLatin1Char('/')) || lexicallyEscapes(pathText)) {
        t.kind = Target::OutOfRoot;                      // step 0, zero stats
        return t;
    }
    if (pathText.contains(QLatin1Char('/')))             // step 1, terminal both ways
        return statAndGate(root, pathText, opts);

    const auto hit = opts.basenameIndex.constFind(pathText);
    if (hit != opts.basenameIndex.constEnd() && !hit->isEmpty()) {
        if (hit->size() >= 2) {                          // step 2b
            t.kind       = Target::Ambiguous;
            t.candidates = *hit;
            // UTF-16 code unit, never locale collation: CI runs under C.UTF-8
            // and a developer's desktop does not, and a locale-sensitive sort
            // would make the cap select a different set on the two machines.
            std::sort(t.candidates.begin(), t.candidates.end());
            return t;
        }
        // Step 2's else is missing_file, not unresolvable: the index asserted a
        // specific path, so its absence is a definite claim about a file.
        return statAndGate(root, hit->first(), opts);
    }
    Target step3 = statAndGate(root, pathText, opts);    // step 3, repo-root file
    if (step3.kind != Target::MissingFile) return step3;
    t.kind = Target::Unresolvable;                       // step 4
    return t;
}

// The most recent citation that can lend its target to a bare continuation.
struct Antecedent {
    bool        valid     = false;
    bool        ambiguous = false;
    QString     relPath;
    QStringList candidates;
};

struct PendingUnparsed {
    int     docLine = 0;
    int     docCol  = 0;
    QString raw;
    bool    rawClipped = false;
    QString reason;
};

QString relativeTo(const QString &root, const QString &abs) {
    if (abs.startsWith(root + QLatin1Char('/'))) return abs.mid(root.size() + 1);
    return abs;
}

QJsonObject refusal(const QString &code) {
    return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("code"), code}};
}

}  // namespace

QJsonObject check(const QString &rootCanonical, const QString &docAbsPath,
                  const Options &opts) {
    QStringList lines;
    const QFileInfo docInfo(docAbsPath);
    if (docInfo.exists() && docInfo.isFile()) {
        QFile f(docAbsPath);
        if (!f.open(QIODevice::ReadOnly)) return refusal(QStringLiteral("read_failed"));
        QString text;
        if (!decodeChecked(f.readAll(), &text)) return refusal(QStringLiteral("read_failed"));
        lines = splitLines(text);
    }

    const int docLines = lines.size();
    const int scanned  = qBound(0, opts.maxDocLines, docLines);
    const QStringList prefix = lines.mid(0, scanned);

    const ScanResult    sc    = scan(prefix, opts);
    const QVector<bool> fence = MarkdownScan::fenceMask(prefix);

    TargetReader           reader(opts);
    QVector<QJsonObject>   entries;
    QVector<PendingUnparsed> unparsedRows;
    QHash<QString, int>    tally;
    Antecedent             ante;
    int                    fenceCursor = 0;

    for (const Citation &tok : sc.citations) {
        // A fence is a context break, not merely skipped text: a `:45` after a
        // code sample far more likely discusses that sample, and a visible
        // `unresolved` beats a silent mis-attribution.
        for (; fenceCursor < tok.docLine && fenceCursor < fence.size(); ++fenceCursor)
            if (fence.at(fenceCursor)) ante = Antecedent{};

        Target t;
        bool   inherited = false;
        if (tok.continuation) {
            if (!ante.valid) {
                t.kind = Target::Unresolved;
            } else if (ante.ambiguous) {
                t.kind       = Target::Ambiguous;
                t.candidates = ante.candidates;
                inherited    = true;
            } else {
                // Only the resolved PATH is inherited — the continuation runs the
                // read step itself and takes its own status, so the two entries
                // can never disagree about which file is meant.
                t         = statAndGate(rootCanonical, ante.relPath, opts);
                inherited = true;
            }
        } else {
            t = resolvePath(rootCanonical, tok.path, opts);
        }

        if (t.kind == Target::OutOfRoot || t.kind == Target::Unresolvable) {
            PendingUnparsed u;
            u.docLine    = tok.docLine;
            u.docCol     = tok.docCol;
            u.raw        = tok.raw.left(opts.maxRawChars);
            u.rawClipped = tok.raw.size() > opts.maxRawChars;
            u.reason     = t.kind == Target::OutOfRoot ? QStringLiteral("out_of_root")
                                                       : QStringLiteral("unresolvable");
            unparsedRows.append(u);
            continue;
        }

        QJsonObject e{{QStringLiteral("doc_line"), tok.docLine},
                      {QStringLiteral("raw"), tok.raw},
                      {QStringLiteral("start_line"), tok.startLine},
                      {QStringLiteral("end_line"), tok.endLine}};
        if (tok.approximate) e.insert(QStringLiteral("approximate"), true);
        if (tok.partial) e.insert(QStringLiteral("partial"), true);
        if (inherited) e.insert(QStringLiteral("inherited_path"), true);

        QString status;
        switch (t.kind) {
        case Target::Unresolved:
            status = QStringLiteral("unresolved");
            break;
        case Target::Ambiguous: {
            status = QStringLiteral("ambiguous");
            QJsonArray cand;
            for (int i = 0; i < t.candidates.size() && i < opts.maxCandidates; ++i)
                cand.append(t.candidates.at(i));
            e.insert(QStringLiteral("candidates"), cand);
            e.insert(QStringLiteral("candidates_total"), t.candidates.size());
            if (t.candidates.size() > opts.maxCandidates)
                e.insert(QStringLiteral("truncated_candidates"), true);
            break;
        }
        case Target::MissingFile:
            status = QStringLiteral("missing_file");
            e.insert(QStringLiteral("path"), t.relPath);
            break;
        default: {
            e.insert(QStringLiteral("path"), t.relPath);
            const int cited = tok.endLine - tok.startLine + 1;
            const TargetReader::Result rr =
                reader.read(t.absPath, tok.startLine, qMin(opts.maxRangeLines, cited));
            if (!rr.ok) {
                status = QStringLiteral("read_error");
                break;
            }
            e.insert(QStringLiteral("file_lines"), rr.fileLines);
            if (tok.startLine > rr.fileLines) {
                // Only start_line decides this, and an empty target needs no
                // special case: file_lines is 0, so line 1 is already past it.
                status = QStringLiteral("out_of_range");
                break;
            }
            status = QStringLiteral("ok");
            QJsonArray text;
            bool escaped = false;
            for (const QString &l : rr.lines) {
                text.append(l);
                escaped = escaped || needsEscaping(l);
            }
            e.insert(QStringLiteral("text"), text);
            if (tok.endLine > rr.fileLines) e.insert(QStringLiteral("end_clamped"), true);
            if (qMin(tok.endLine, rr.fileLines) - tok.startLine + 1 > opts.maxRangeLines)
                e.insert(QStringLiteral("range_truncated"), true);
            if (rr.clipped) e.insert(QStringLiteral("text_clipped"), true);
            if (escaped) e.insert(QStringLiteral("text_escaped"), true);
            break;
        }
        }

        e.insert(QStringLiteral("status"), status);
        ++tally[status];
        entries.append(e);

        if (t.kind != Target::Unresolved) {
            ante.valid     = true;
            ante.ambiguous = (t.kind == Target::Ambiguous);
            ante.relPath   = t.relPath;
            ante.candidates = t.candidates;
        }
    }

    for (const Unparsed &u : sc.unparsed)
        unparsedRows.append({u.docLine, u.docCol, u.raw, u.rawClipped, u.reason});
    std::stable_sort(unparsedRows.begin(), unparsedRows.end(),
                     [](const PendingUnparsed &a, const PendingUnparsed &b) {
                         return a.docLine != b.docLine ? a.docLine < b.docLine
                                                       : a.docCol < b.docCol;
                     });

    const int okCount = tally.value(QStringLiteral("ok"));
    QJsonObject counts;
    for (const char *k : {"ok", "missing_file", "out_of_range", "read_error", "ambiguous",
                          "unresolved"})
        counts.insert(QLatin1String(k), tally.value(QString::fromLatin1(k)));
    // Overlays on the ok subset, not statuses. Until ANTS-3654 lands the anchor
    // check nothing is judged, so every ok citation is `unchecked` — which is
    // the honesty field doing its job rather than a placeholder.
    counts.insert(QStringLiteral("anchor_missing"), 0);
    counts.insert(QStringLiteral("unchecked"), okCount);
    counts.insert(QStringLiteral("unparsed"), unparsedRows.size());

    QJsonArray unparsedOut;
    for (int i = 0; i < unparsedRows.size() && i < opts.maxUnparsed; ++i) {
        const PendingUnparsed &u = unparsedRows.at(i);
        QJsonObject o{{QStringLiteral("doc_line"), u.docLine},
                      {QStringLiteral("raw"), u.raw},
                      {QStringLiteral("reason"), u.reason}};
        if (u.rawClipped) o.insert(QStringLiteral("raw_clipped"), true);
        unparsedOut.append(o);
    }

    QJsonObject out{
        {QStringLiteral("ok"), true},
        {QStringLiteral("path"), relativeTo(rootCanonical, docAbsPath)},
        {QStringLiteral("doc_lines"), docLines},
        {QStringLiteral("scanned_lines"), scanned},
        {QStringLiteral("only"), opts.only == Only::Stale ? QStringLiteral("stale")
                                                          : QStringLiteral("all")},
        {QStringLiteral("offset"), opts.offset},
        {QStringLiteral("count"), entries.size()},
        {QStringLiteral("counts"), counts},
        {QStringLiteral("unparsed"), unparsedOut},
        {QStringLiteral("unparsed_total"), unparsedRows.size()},
        {QStringLiteral("basename_index_size"), opts.basenameIndex.size()}};
    if (opts.basenameIndexTruncated)
        out.insert(QStringLiteral("basename_index_truncated"), true);
    if (reader.budgetHit()) out.insert(QStringLiteral("read_budget_exhausted"), true);
    if (sc.unterminatedFence >= 0)
        out.insert(QStringLiteral("unterminated_fence"), sc.unterminatedFence);

    // Filter, then page, then fill until a cap binds — that order is what makes
    // next_offset == offset + returned correct and every entry reachable.
    QVector<QJsonObject> filtered;
    for (const QJsonObject &e : entries)
        if (opts.only == Only::All
            || e.value(QStringLiteral("status")).toString() != QLatin1String("ok"))
            filtered.append(e);

    // The budget is what is left after everything that is never trimmed —
    // unparsed[] above all, which max_bytes must not touch.
    const qint64 baseBytes = QJsonDocument(out).toJson(QJsonDocument::Compact).size();
    const qint64 budget    = qMax<qint64>(0, opts.maxBytes - baseBytes);
    QJsonArray page;
    qint64     used         = 0;
    bool       capTruncated = false;
    for (int i = opts.offset; i < filtered.size(); ++i) {
        if (page.size() >= opts.maxCitations) { capTruncated = true; break; }
        const qint64 sz = QJsonDocument(filtered.at(i)).toJson(QJsonDocument::Compact).size();
        // INV-35: one entry always ships. A paging caller that receives zero
        // entries with next_offset == offset loops forever, so max_bytes is a
        // soft ceiling with one bounded overshoot.
        if (!page.isEmpty() && used + sz > budget) { capTruncated = true; break; }
        used += sz;
        page.append(filtered.at(i));
    }

    out.insert(QStringLiteral("citations"), page);
    out.insert(QStringLiteral("returned"), page.size());
    // Emitted exactly when a RESUMABLE cap dropped trailing entries. A
    // scan-level cap that also bound does not suppress it: those entries were
    // found, and stranding them is the silent shortening this verb forbids.
    if (capTruncated) out.insert(QStringLiteral("next_offset"), opts.offset + page.size());
    out.insert(QStringLiteral("truncated"),
               capTruncated || scanned < docLines || unparsedRows.size() > opts.maxUnparsed);
    return out;
}

}  // namespace DocCitations
