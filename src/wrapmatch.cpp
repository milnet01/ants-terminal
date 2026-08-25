#include "wrapmatch.h"

#include <QChar>
#include <QLatin1Char>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QStringList>

namespace WrapMatch {
namespace {

// Escape the regex metacharacters and NOTHING else. QRegularExpression::
// escape() backslashes every non-word character, non-ASCII included — and
// `\—` is an error in ripgrep's Rust regex, which would turn an em dash in
// a quotation (common in this corpus) into a refusal at the far end. The
// two engines accept the same escapes for this set, so one escaped token
// drives both.
QString escapeLiteral(const QString &s) {
    static const QString kMeta = QStringLiteral("\\.^$|()[]{}*+?-/#&~");
    QString out;
    out.reserve(s.size() * 2);
    for (const QChar c : s) {
        if (kMeta.contains(c)) out.append(QLatin1Char('\\'));
        out.append(c);
    }
    return out;
}

// The separator a whitespace run in the needle becomes: at least one
// whitespace character, then any number of CONTINUATION MARKERS with their
// trailing space. That is the "and blockquote markers" half of the rule, and
// it is what makes the normalisation two-sided — a quotation pasted with its
// own prefixes tokenises them away (below) while the FILE keeps them here.
//
// ANTS-4607 — a source COMMENT LEADER is such a marker and was not folded, so
// a sentence hard-wrapped across `//` lines could not be found at all. That is
// most of what a spec quotes: a C++ codebase keeps its reasoning in wrapped
// comments, and a review gate that dismisses a finding whose quotation it
// cannot locate cannot tell "absent" from "wrapped" — so the real defect
// ships. Measured on this repo: fillBulletRecord()'s comment in
// roadmapparse.cpp carries such a sentence verbatim over three lines, and it
// returned zero matches while the same sentence was found in two .md copies.
//
// `//` and `#` are the line-comment leaders; a lone `*` is the javadoc/doxygen
// continuation. `\\*` is deliberately NOT `\\*+`: one asterisk is the
// continuation marker, while `**` opens markdown bold, and folding that away
// would let a needle skip emphasis that is really there.
const char *kSeparator = "[ \\t\\r\\n]+(?:(?:>+|/{2,}|#+|\\*)[ \\t]*)*";

// Split the needle into the literal runs the separator joins. A token that is
// nothing but continuation markers is dropped rather than matched: it is the
// marker of a wrapped line in the PASTED quotation, not text.
//
// ANTS-4607 — this is the second side of the separator above, and the two must
// carry the same marker set or the normalisation stops being two-sided: paste
// a comment block complete with its `//` leaders and they tokenise away here,
// exactly as `>` already did.
bool isMarkerOnly(const QString &tok) {
    for (const QChar c : tok) {
        if (c != QLatin1Char('>') && c != QLatin1Char('/')
            && c != QLatin1Char('#') && c != QLatin1Char('*'))
            return false;
    }
    return true;
}

QStringList tokenise(const QString &needle) {
    QStringList out;
    for (const QString &tok :
         needle.split(QRegularExpression(QStringLiteral("[ \\t\\r\\n]+")),
                      Qt::SkipEmptyParts)) {
        if (!isMarkerOnly(tok)) out.append(tok);
    }
    return out;
}

}  // namespace

QString toRegex(const QString &needle) {
    const QStringList tokens = tokenise(needle);
    if (tokens.isEmpty()) return {};
    QStringList escaped;
    escaped.reserve(tokens.size());
    for (const QString &t : tokens) escaped.append(escapeLiteral(t));
    return escaped.join(QString::fromLatin1(kSeparator));
}

QVector<Span> find(const QString &haystack, const QString &needle,
                   int maxHits) {
    QVector<Span> out;
    const QString pattern = toRegex(needle);
    if (pattern.isEmpty() || haystack.isEmpty()) return out;
    const QRegularExpression rx(pattern);
    if (!rx.isValid()) return out;
    auto it = rx.globalMatch(haystack);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out.append(Span{static_cast<int>(m.capturedStart()),
                        static_cast<int>(m.capturedLength())});
        if (maxHits > 0 && out.size() >= maxHits) break;
    }
    return out;
}

Patch patchOnce(const QString &text, const QString &oldText,
                const QString &newText, Indent indent) {
    Patch p;
    if (oldText.isEmpty()) return p;

    // Pass 1 — exact. Unchanged from the single-line matcher this
    // replaces: same occurrences, same count, same uniqueness verdict.
    int start = -1;
    for (int at = text.indexOf(oldText); at >= 0;
         at = text.indexOf(oldText, at + oldText.size())) {
        if (p.hits == 0) start = at;
        ++p.hits;
    }
    int length = oldText.size();

    // Pass 2 — wrapped, and ONLY on an exact miss, so the fallback can
    // turn a refusal into an edit and can never redirect an edit that
    // already had a home.
    if (p.hits == 0) {
        const QVector<Span> spans = find(text, oldText);
        p.hits = static_cast<int>(spans.size());
        if (p.hits >= 1) {
            start   = spans.at(0).start;
            length  = spans.at(0).length;
            p.wrapped = true;
        }
    }
    if (p.hits != 1) return p;   // 0 -> miss, >1 -> ambiguous

    p.line = text.left(start).count(QLatin1Char('\n'));

    // ANTS-4612 — a wrapped hit re-flows the lines it spanned into one, which
    // is right for a HARD WRAP and destroys anything else. Measured on
    // CFG-0196: three successive amend_body calls, each ok:true with the
    // correct text echoed, walked one row of an aligned block from 4 to 6 to 8
    // to 12 leading spaces. Cumulative, because every repair attempt ran this
    // same pass — and on a store-backed project the file is a render, so hand
    // repair is reverted: this verb was both the only way back and the thing
    // causing the damage.
    //
    // The reported discriminator was "the span's lines are indented
    // differently", and that is WRONG — ANTS-3467's fixture disproves it:
    //
    //     - **Auto-lock timeout (the priority):** make it
    //       user-configurable (e.g. 1/5/10/30 min) so users tune it.
    //
    // is 2 spaces then 4, and is an ordinary hard wrap; the deeper indent is
    // the bullet's hanging indent. Declining on indent alone breaks it.
    //
    // What actually separates the two is STRUCTURE on a continuation line:
    //   (a) an internal run of 2+ spaces — column alignment, which a re-flow
    //       silently destroys and which prose never has; or
    //   (b) a line that opens a new list item — the span has crossed a
    //       structural boundary, so the two lines are siblings, not a wrap.
    // Neither is true of a hard-wrapped sentence, and both are true of exactly
    // the blocks a re-flow ruins.
    if (p.wrapped) {
        static const QRegularExpression reAligned(QStringLiteral("\\S[ \\t]{2,}\\S"));
        static const QRegularExpression reListItem(
            QStringLiteral("\\A[ \\t]*(?:[-*+]|\\d+[.)])\\s"));
        const int endOff = start + length;
        int bol = text.indexOf(QLatin1Char('\n'), start) + 1;   // 2nd line on
        for (; bol > 0 && bol < endOff; ) {
            int eol = text.indexOf(QLatin1Char('\n'), bol);
            if (eol < 0) eol = text.size();
            const QString line = text.mid(bol, eol - bol);
            if (line.contains(reAligned) || line.contains(reListItem)) {
                p.structuredBlock = true;
                return p;   // `text` stays empty — no half-apply
            }
            bol = eol + 1;
        }
    }

    // ANTS-3752 — a multi-line `newText` must not land flush-left where
    // the surrounding text is indented continuation. Give each of its
    // continuation lines the matched line's own indent; relative
    // indentation the caller supplied is PRESERVED (prefix, not replace).
    QString patch = newText;
    if (indent == Indent::MatchLineIndent &&
        patch.contains(QLatin1Char('\n'))) {
        const int lineStart = text.lastIndexOf(QLatin1Char('\n'), start) + 1;
        int w = lineStart;
        while (w < text.size() && text.at(w).isSpace() &&
               text.at(w) != QLatin1Char('\n')) ++w;
        const QString lead = text.mid(lineStart, w - lineStart);
        if (!lead.isEmpty()) {
            QStringList parts = patch.split(QLatin1Char('\n'));
            for (int i = 1; i < parts.size(); ++i)
                if (!parts.at(i).isEmpty()) parts[i].prepend(lead);
            patch = parts.join(QLatin1Char('\n'));
        }
    }

    p.text = text;
    p.text.replace(start, length, patch);
    return p;
}

}  // namespace WrapMatch
