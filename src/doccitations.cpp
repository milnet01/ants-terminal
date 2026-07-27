// ANTS-3653 — see doccitations.h for the contract, docs/specs/ANTS-3653.md for
// the grammar.

#include "doccitations.h"

#include "markdownscan.h"

#include <QChar>
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
