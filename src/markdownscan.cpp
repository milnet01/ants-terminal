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

QChar fenceOpenerChar(const QString &line) {
    const auto m = fenceRe().match(line);
    if (!m.hasMatch()) return QChar();
    return m.captured(1).at(0);
}

QVector<bool> fenceMask(const QStringList &lines) {
    QVector<bool> mask(lines.size(), false);
    QChar openFence;  // null when not inside a fence
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!openFence.isNull()) {
            // Inside a fence — the opener, body, and closer all mask true.
            mask[i] = true;
            const QChar c = fenceOpenerChar(line);
            if (!c.isNull() && c == openFence) openFence = QChar();
            continue;
        }
        const QChar opener = fenceOpenerChar(line);
        if (!opener.isNull()) { openFence = opener; mask[i] = true; }
    }
    return mask;
}

}  // namespace MarkdownScan
