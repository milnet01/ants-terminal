// ANTS-3603 — see markdownscan.h for the contract.

#include "markdownscan.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QString>
#include <QStringList>

namespace MarkdownScan {

const QRegularExpression &fenceRe() {
    // ANTS-3655 — the negative lookahead is CommonMark's rule that a BACKTICK
    // fence's info string may not contain a backtick. Greedy `+` takes the
    // whole run and cannot usefully backtrack (dropping a backtick from the run
    // only puts one back into the remainder), so this accepts exactly what
    // fenceOpenerChar's run-then-check hand-scan does. Input is one line, so
    // `.` needing no DOTALL is the same assumption every caller already makes.
    static const QRegularExpression re(
        QStringLiteral("^ {0,3}(```+(?!.*`)|~~~+)"));
    return re;
}

namespace {

int leadingSpaces(const QString &line) {
    int n = 0;
    while (n < line.size() && line.at(n) == QLatin1Char(' ')) ++n;
    return n;
}

// ANTS-3638 — a list-item marker, whose match width is the item's content
// column. `- `, `* `, `+ `, `1. `, `1) `; CommonMark's 9-digit ordinal cap.
const QRegularExpression &listMarkerRe() {
    static const QRegularExpression re(
        QStringLiteral("^( *)([-*+]|\\d{1,9}[.)])( +)"));
    return re;
}

}  // namespace

QChar fenceOpenerChar(const QString &line, int maxIndent) {
    // Hand-scanned rather than regex-matched because maxIndent varies
    // (ANTS-3638). At the default 3 this accepts exactly what fenceRe()
    // matches, which is the contract INV-1 pins — leading SPACES only, so a
    // tab-indented line still never opens a fence.
    const int ind = leadingSpaces(line);
    if (ind > maxIndent || line.size() - ind < 3) return QChar();
    const QChar c = line.at(ind);
    if (c != QLatin1Char('`') && c != QLatin1Char('~')) return QChar();
    if (line.at(ind + 1) != c || line.at(ind + 2) != c) return QChar();
    // ANTS-3655 — a backtick fence's info string may not contain a backtick
    // (CommonMark § 4.5). Without this a line that is really a multi-backtick
    // inline code span — ```` ```` ``` ```` ````, how a doc quotes fence syntax
    // — opened a fence that never closed and masked the document to its end.
    // Tilde fences are exempt: their info string may hold any character but a
    // tilde run, which the run scan below already consumed.
    if (c == QLatin1Char('`')) {
        int j = ind + 3;
        while (j < line.size() && line.at(j) == c) ++j;   // rest of the run
        if (line.indexOf(c, j) >= 0) return QChar();      // backtick in the info
    }
    return c;
}

QVector<bool> fenceMask(const QStringList &lines) {
    return fenceMask(lines, nullptr);
}

QVector<bool> fenceMask(const QStringList &lines, int *unterminatedOpenerLine) {
    if (unterminatedOpenerLine) *unterminatedOpenerLine = -1;
    QVector<bool> mask(lines.size(), false);
    QChar openFence;         // null when not inside a fence
    int   openLine      = -1;// 1-based line of the open fence's opener
    int   openAllowance = 3; // indent the closer of the open fence may carry
    // Content columns of the open list items, outermost → innermost. Empty
    // at top level, where the allowance is CommonMark's plain 3 spaces.
    QVector<int> listCols;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!openFence.isNull()) {
            // Inside a fence — the opener, body, and closer all mask true.
            // The container stack is frozen: a bullet inside a code sample
            // is sample text, not a list.
            mask[i] = true;
            const QChar c = fenceOpenerChar(line, openAllowance);
            if (!c.isNull() && c == openFence) { openFence = QChar(); openLine = -1; }
            continue;
        }
        if (!line.trimmed().isEmpty()) {
            // A line indented less than the innermost item's content column
            // has left that item (blank lines do not — a list item may span
            // them). Then a marker line opens the next container.
            const int ind = leadingSpaces(line);
            while (!listCols.isEmpty() && ind < listCols.last())
                listCols.removeLast();
            const auto lm = listMarkerRe().match(line);
            if (lm.hasMatch()) listCols.append(lm.capturedLength());
        }
        const int allowance =
            (listCols.isEmpty() ? 0 : listCols.last()) + 3;
        const QChar opener = fenceOpenerChar(line, allowance);
        if (!opener.isNull()) {
            openFence     = opener;
            openLine      = i + 1;
            openAllowance = allowance;
            mask[i]       = true;
        }
    }
    // Only the OUTERMOST unclosed opener: a fence char inside an open fence is
    // body text, so `openLine` can only be the one that never closed.
    if (unterminatedOpenerLine && !openFence.isNull())
        *unterminatedOpenerLine = openLine;
    return mask;
}

QVector<CodeSpan> codeSpans(const QStringList &lines,
                            const QVector<bool> &fence) {
    QVector<CodeSpan> out;
    const auto isBoundary = [&](int li) {
        return fence.value(li) || lines.at(li).trimmed().isEmpty();
    };
    for (int li = 0; li < lines.size(); ++li) {
        if (isBoundary(li)) continue;
        int i = 0;
        while (i < lines.at(li).size()) {
            if (lines.at(li).at(i) != QLatin1Char('`')) { ++i; continue; }
            const int openStart = i;
            int openLen = 0;
            while (i < lines.at(li).size()
                   && lines.at(li).at(i) == QLatin1Char('`')) { ++i; ++openLen; }

            // Forward search for a run of EQUAL length, never past a blank or
            // fenced line: an inline span crosses neither.
            int closeLine = li, cj = i, closeEnd = -1;
            for (;;) {
                const QString &s = lines.at(closeLine);
                while (cj < s.size()) {
                    if (s.at(cj) != QLatin1Char('`')) { ++cj; continue; }
                    int runLen = 0;
                    while (cj < s.size() && s.at(cj) == QLatin1Char('`')) {
                        ++cj;
                        ++runLen;
                    }
                    if (runLen == openLen) { closeEnd = cj; break; }
                }
                if (closeEnd >= 0 || closeLine + 1 >= lines.size()
                    || isBoundary(closeLine + 1)) break;
                ++closeLine;
                cj = 0;
            }
            if (closeEnd < 0) continue;   // unmatched run — literal text

            out.append({li, openStart + openLen,
                        closeLine, closeEnd - openLen, openLen});
            if (closeLine == li) {
                i = closeEnd;             // resume just past the closing run
                continue;
            }
            // Multi-line span: the rest of this line is inside it, so resume
            // on the closing line, past its closing run.
            li = closeLine;
            i  = closeEnd;
        }
    }
    return out;
}

namespace {

// ANTS-3659 § 2.1. Anchored at both ends, so the marker must be alone on its
// line: a marker sharing a line with prose would leave "is the prose before it
// inside or outside?" with no non-arbitrary answer. The indent is space-only
// for fenceRe's reason (ANTS-3598) — a `\s` class admits `\r`, and a stray
// carriage return would then open a region.
const QRegularExpression &exampleMarkerRe() {
    static const QRegularExpression re(QStringLiteral(
        "^ {0,3}<!--[ \\t]*doc-examples:[ \\t]*(begin|end)[ \\t]*-->[ \\t]*$"));
    return re;
}

}  // namespace

QVector<bool> exampleMask(const QStringList &lines, const QVector<bool> &fence,
                          int *unterminatedOpenerLine) {
    Q_ASSERT(fence.size() == lines.size());

    QVector<bool> mask(lines.size(), false);
    int           open = -1;   // 0-based line of the FIRST unclosed begin, or -1

    const auto fill = [&mask](int from, int to) {
        for (int i = from; i <= to; ++i) mask[i] = true;
    };

    for (int li = 0; li < lines.size(); ++li) {
        if (fence.value(li)) continue;          // fenced ⇒ sample text, not syntax
        const QString &line = lines.at(li);
        // Prefilter: the marker-free document is the common case and an anchored
        // regex per line is this primitive's whole cost. Not a contract — the
        // regex alone is authoritative.
        if (!line.contains(QLatin1String("<!--"))) continue;
        const QRegularExpressionMatch m = exampleMarkerRe().match(line);
        if (!m.hasMatch()) continue;

        if (m.capturedView(1) == QLatin1String("begin")) {
            if (open < 0) open = li;            // no nesting: keep the FIRST opener
        } else if (open >= 0) {
            fill(open, li);
            open = -1;
        }
        // An `end` with no open region is ignored, and its line is not masked.
    }

    // Unterminated: mask to the end of the INPUT, matching fenceMask's
    // CommonMark leniency. For a truncated scan that is the prefix, not the
    // document — see the header.
    if (open >= 0) fill(open, lines.size() - 1);
    if (unterminatedOpenerLine) *unterminatedOpenerLine = open < 0 ? -1 : open + 1;
    return mask;
}

int headingLevel(const QString &trimmedLine) {
    int h = 0;
    while (h < trimmedLine.size() && trimmedLine.at(h) == QLatin1Char('#')) ++h;
    if (h >= 1 && h <= 6 &&
        (h == trimmedLine.size() || trimmedLine.at(h) == QLatin1Char(' ')))
        return h;
    return 0;
}

QString headingSlug(const QString &text) {
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

QString stripBlockquote(const QString &trimmedLine, int *depth) {
    int d = 0;
    int i = 0;
    while (i < trimmedLine.size()) {
        int j = i;
        int spaces = 0;
        while (j < trimmedLine.size() && spaces < 3 &&
               trimmedLine.at(j) == QLatin1Char(' ')) { ++j; ++spaces; }
        if (j >= trimmedLine.size() || trimmedLine.at(j) != QLatin1Char('>')) break;
        ++d;
        i = j + 1;
        // CommonMark § 5.1: one space after the marker is part of the marker.
        if (i < trimmedLine.size() && trimmedLine.at(i) == QLatin1Char(' ')) ++i;
    }
    if (depth) *depth = d;
    return d > 0 ? trimmedLine.mid(i) : trimmedLine;
}

QVector<Heading> headings(const QStringList &lines) {
    QVector<Heading> out;
    const QVector<bool> fence = fenceMask(lines);
    for (int i = 0; i < lines.size(); ++i) {
        if (fence.value(i)) continue;   // opener, closer and body are all masked
        const QString trimmed = lines.at(i).trimmed();
        // ANTS-4520 — a blockquoted heading is a heading. The fence mask above
        // still wins, so `> ## x` inside a fenced block stays sample text.
        int quoteDepth = 0;
        const QString content = stripBlockquote(trimmed, &quoteDepth);
        const int level = headingLevel(content);
        if (level == 0) continue;
        const QString text = content.mid(level).trimmed();
        out.push_back({i + 1, level, text, headingSlug(text), 0, quoteDepth});
    }
    // Second pass for the spans: a section ends at the line before the next
    // heading that TERMINATES it, else at the last input line. A heading
    // terminates an earlier one when it is less deeply quoted (leaving the
    // quote ends the quoted section — ANTS-4520), or equally quoted at the
    // same or a higher level (the original rule, which is what depth 0 to
    // depth 0 reduces to). A MORE deeply quoted heading is nested inside.
    for (int i = 0; i < out.size(); ++i) {
        out[i].endLine = lines.size();
        for (int j = i + 1; j < out.size(); ++j) {
            const bool terminates =
                out[j].quoteDepth < out[i].quoteDepth ||
                (out[j].quoteDepth == out[i].quoteDepth && out[j].level <= out[i].level);
            if (terminates) { out[i].endLine = out[j].line - 1; break; }
        }
    }
    return out;
}

}  // namespace MarkdownScan
