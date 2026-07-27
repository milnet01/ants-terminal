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

}  // namespace MarkdownScan
