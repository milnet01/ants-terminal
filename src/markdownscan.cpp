// ANTS-3603 — see markdownscan.h for the contract.

#include "markdownscan.h"

#include <algorithm>
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

QChar fenceOpenerChar(const QString &line, int maxIndent, int *runLength) {
    if (runLength) *runLength = 0;
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
    int j = ind + 3;
    while (j < line.size() && line.at(j) == c) ++j;       // rest of the run
    if (c == QLatin1Char('`')) {
        if (line.indexOf(c, j) >= 0) return QChar();      // backtick in the info
    }
    if (runLength) *runLength = j - ind;
    return c;
}

bool fenceCloses(const QString &line, QChar openChar, int openRun,
                 int maxIndent) {
    if (openChar.isNull()) return false;
    int run = 0;
    const QChar c = fenceOpenerChar(line, maxIndent, &run);
    return !c.isNull() && c == openChar && run >= openRun;
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
    // ANTS-3678 — CommonMark § 4.5: the closer must be at least as long as
    // the opener. Without the run length a ``` line ended a ```` block, and
    // everything after it — headings included — leaked out of the sample as
    // prose. A document quoting fence syntax is the ordinary way to hit it.
    int   openRun       = 0; // length of the open fence's character run
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
            int run = 0;
            const QChar c = fenceOpenerChar(line, openAllowance, &run);
            if (!c.isNull() && c == openFence && run >= openRun) {
                openFence = QChar();
                openLine  = -1;
                openRun   = 0;
            }
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
        int openerRun = 0;
        const QChar opener = fenceOpenerChar(line, allowance, &openerRun);
        if (!opener.isNull()) {
            openFence     = opener;
            openLine      = i + 1;
            openAllowance = allowance;
            openRun       = openerRun;
            mask[i]       = true;
        }
    }
    // Only the OUTERMOST unclosed opener: a fence char inside an open fence is
    // body text, so `openLine` can only be the one that never closed.
    if (unterminatedOpenerLine && !openFence.isNull())
        *unterminatedOpenerLine = openLine;
    return mask;
}

// ANTS-4598 — two passes, and the order between them is the fix. Pass 1 pairs
// runs WITHIN a line; pass 2 joins what is left over across lines.
//
// The single forward sweep this replaced paired runs in document order, so a
// run left over on one line took as its closer the OPENER of a balanced line
// further down. From there the polarity is inverted for the rest of the body:
// what the author quoted reads as prose, and what they wrote as prose reads as
// quoted. For `roadmapparse`'s mask that means a quoted trailer key downstream
// is read as a declaration, which is where the defect was measured.
//
// Pairing within a line first is what separates the two shapes the sweep could
// not tell apart. A hard-wrapped span leaves a leftover run on BOTH lines and
// still joins in pass 2; a line that balances on its own keeps its runs and has
// none to donate. Measured 2026-08-20 over the machine-global store's 4291
// bodies: four bullets parse differently, all four toward the declaration their
// author wrote, and none loses a value.
//
// This is not CommonMark § 6.1's order, which pairs left to right across the
// whole paragraph. The primitive is a MASK — it decides where a span is, to
// keep a quotation from being read as a declaration — and on ambiguous input
// (`` `a `b` c` ``) CommonMark's answer and this one are equally arbitrary.
// The 409 legitimate wrapped spans in that corpus are the case worth keeping,
// and pass 2 keeps them.
QVector<CodeSpan> codeSpans(const QStringList &lines,
                            const QVector<bool> &fence) {
    QVector<CodeSpan> out;
    const auto isBoundary = [&](int li) {
        return fence.value(li) || lines.at(li).trimmed().isEmpty();
    };

    // Every backtick run outside a boundary line, in document order.
    struct Run { int line, col, len; bool used; };
    QVector<Run> runs;
    for (int li = 0; li < lines.size(); ++li) {
        if (isBoundary(li)) continue;
        const QString &s = lines.at(li);
        int i = 0;
        while (i < s.size()) {
            if (s.at(i) != QLatin1Char('`')) { ++i; continue; }
            const int start = i;
            int len = 0;
            while (i < s.size() && s.at(i) == QLatin1Char('`')) { ++i; ++len; }
            runs.append({li, start, len, false});
        }
    }

    // Pairing (a, b) also consumes every run BETWEEN them: those runs are span
    // CONTENT, not delimiters, so a ``` quoted inside a `…` span can never
    // open one of its own. The sweep got this by resuming at the closing run;
    // with the runs collected up front it has to be said, and a draft that did
    // not say it lost the trailers of two bullets quoting a fence pattern.
    const auto pair = [&](int a, int b) {
        for (int k = a; k <= b; ++k) runs[k].used = true;
        out.append({runs[a].line, runs[a].col + runs[a].len,
                    runs[b].line, runs[b].col, runs[a].len});
    };

    // Pass 1 — within one line. `a = b` resumes past the closer, as the sweep
    // did, so the next opener is looked for outside the span just closed.
    for (int a = 0; a < runs.size(); ++a) {
        if (runs.at(a).used) continue;
        int b = a + 1;
        while (b < runs.size() && runs.at(b).line == runs.at(a).line) {
            if (!runs.at(b).used && runs.at(b).len == runs.at(a).len) break;
            ++b;
        }
        if (b >= runs.size() || runs.at(b).line != runs.at(a).line) continue;
        pair(a, b);
        a = b;
    }

    // Pass 2 — join the leftovers across lines. CommonMark § 6.1's newline
    // allowance, kept: a span may cross a line break but never a blank or a
    // fenced line, so the search stops at the first boundary rather than
    // skipping it. A run that finds no partner here is literal text and opens
    // nothing — which is what makes one stray backtick harmless.
    for (int a = 0; a < runs.size(); ++a) {
        if (runs.at(a).used) continue;
        for (int b = a + 1; b < runs.size(); ++b) {
            if (runs.at(b).line == runs.at(a).line) continue;
            bool blocked = false;
            for (int l = runs.at(a).line + 1; l <= runs.at(b).line; ++l)
                if (isBoundary(l)) { blocked = true; break; }
            if (blocked) break;
            if (runs.at(b).used || runs.at(b).len != runs.at(a).len) continue;
            pair(a, b);
            break;
        }
    }

    // Pass 2 appends out of order; consumers read spans in document order.
    std::sort(out.begin(), out.end(), [](const CodeSpan &x, const CodeSpan &y) {
        return x.startLine != y.startLine ? x.startLine < y.startLine
                                          : x.startCol < y.startCol;
    });
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
