// ANTS-2022 — apply_edits pure in-memory edit helper. Qt6::Core-only.
// A whole-content substring replace per edit; the wrapper threads one
// edit's result into the next so two edits to one file compose, then
// writes atomically. See docs/specs/ANTS-2022.md.

#include "applyedits.h"

#include <QStringList>

namespace ApplyEdits {

EditOutcome applyToContent(const QString &contents, const QString &oldStr,
                           const QString &newStr, bool replaceAll) {
    EditOutcome r;
    // An empty `old` is rejected upstream (bad_args); guard defensively so
    // QString::count("") (which returns size()+1) can't be misread as a hit.
    if (oldStr.isEmpty()) {
        r.skipReason = QStringLiteral("not_found");
        return r;
    }
    const int count = contents.count(oldStr);
    if (count == 0) {
        r.skipReason = QStringLiteral("not_found");
        return r;
    }
    if (count > 1 && !replaceAll) {
        r.skipReason = QStringLiteral("ambiguous");
        return r;
    }

    QString out = contents;
    if (replaceAll) {
        out.replace(oldStr, newStr);
        r.replacements = count;
    } else {
        const int idx = out.indexOf(oldStr);
        out.replace(idx, oldStr.size(), newStr);
        r.replacements = 1;
    }
    r.applied = true;
    r.newContents = out;
    return r;
}

EditOutcome applyRangeToContent(const QString &contents, int startLine,
                                int endLine, const QString &expectFirst,
                                const QString &expectLast,
                                const QString &newStr) {
    EditOutcome r;
    QStringList lines = contents.split(QLatin1Char('\n'));
    if (startLine < 1 || endLine < startLine || endLine > lines.size()) {
        r.skipReason = QStringLiteral("range_out_of_bounds");
        return r;
    }
    // Verbatim, both ends. Checking only one end would let a range that has
    // grown or shrunk at the other end through — the commonest drift there is.
    if (lines.at(startLine - 1) != expectFirst ||
        lines.at(endLine - 1) != expectLast) {
        r.skipReason = QStringLiteral("range_mismatch");
        return r;
    }

    const int count = endLine - startLine + 1;
    // An empty replacement removes the lines outright; splicing `{""}` would
    // leave a blank line where the caller asked for nothing.
    const QStringList repl = newStr.isEmpty()
                                 ? QStringList()
                                 : newStr.split(QLatin1Char('\n'));
    auto first = lines.begin() + (startLine - 1);
    lines.erase(first, first + count);
    for (int i = 0; i < repl.size(); ++i)
        lines.insert(startLine - 1 + i, repl.at(i));

    r.applied      = true;
    r.replacements = 1;
    r.newContents  = lines.join(QLatin1Char('\n'));
    return r;
}

}  // namespace ApplyEdits
