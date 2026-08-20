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
// whitespace character, then any number of markdown blockquote markers
// with their trailing space. That is the "and blockquote markers" half of
// the rule, and it is what makes the normalisation two-sided — a
// quotation pasted with its own `>` prefixes tokenises them away (below)
// while the FILE keeps them here.
const char *kSeparator = "[ \\t\\r\\n]+(?:>+[ \\t]*)*";

// Split the needle into the literal runs the separator joins. A token
// that is nothing but blockquote markers is dropped rather than matched:
// it is the marker of a wrapped line in the pasted quotation, not text.
QStringList tokenise(const QString &needle) {
    QStringList out;
    for (const QString &tok :
         needle.split(QRegularExpression(QStringLiteral("[ \\t\\r\\n]+")),
                      Qt::SkipEmptyParts)) {
        bool markerOnly = true;
        for (const QChar c : tok)
            if (c != QLatin1Char('>')) { markerOnly = false; break; }
        if (!markerOnly) out.append(tok);
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
