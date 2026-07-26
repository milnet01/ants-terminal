// ANTS-3603 — see markdownscan.h for the contract.

#include "markdownscan.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QString>
#include <QStringList>

namespace MarkdownScan {

const QRegularExpression &fenceRe() {
    static const QRegularExpression re(
        QStringLiteral("^ {0,3}(```|~~~)"));
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
    return c;
}

QVector<bool> fenceMask(const QStringList &lines) {
    QVector<bool> mask(lines.size(), false);
    QChar openFence;         // null when not inside a fence
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
            if (!c.isNull() && c == openFence) openFence = QChar();
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
            openAllowance = allowance;
            mask[i]       = true;
        }
    }
    return mask;
}

}  // namespace MarkdownScan
