// ANTS-2022 — apply_edits pure in-memory edit helper. Qt6::Core-only.
// A whole-content substring replace per edit; the wrapper threads one
// edit's result into the next so two edits to one file compose, then
// writes atomically. See docs/specs/ANTS-2022.md.

#include "applyedits.h"

#include <QStringList>

namespace ApplyEdits {

namespace {

// ANTS-4418 — collapse runs of horizontal whitespace to one space and drop
// trailing whitespace, so two spellings that differ only in spacing compare
// equal. Deliberately NOT a full normalisation: newlines are left alone, since
// they are what the line split below is keyed on.
QString wsNormalised(const QString &s) {
    QString out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (const QChar c : s) {
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (pendingSpace) { out.append(QLatin1Char(' ')); pendingSpace = false; }
        out.append(c);
    }
    return out;
}

// ANTS-4418 — locate the single line whose whitespace-normalised form equals
// the normalised `oldStr`. Sets the near-miss fields only on a UNIQUE hit:
// two candidate lines cannot tell the caller which to retry, and naming one
// arbitrarily is worse than naming none.
//
// SCOPE, and it is the reported case rather than the general one: a
// SINGLE-LINE `oldStr`. A multi-line old string whose interior spacing drifted
// needs a windowed alignment over the file, which is a different piece of work
// and not what the round-trip cost was measured on — the report's own repro is
// one line differing by two spaces in a trailing-comment alignment column.
// A multi-line miss therefore reports no near miss, which is honest; the
// alternative is a confident wrong line.
void findWhitespaceNearMiss(const QString &contents, const QString &oldStr,
                            EditOutcome &r) {
    if (oldStr.contains(QLatin1Char('\n'))) return;
    const QString needle = wsNormalised(oldStr);
    if (needle.isEmpty()) return;

    int hitLine = -1;
    QString hitText;
    const QStringList lines = contents.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (wsNormalised(lines.at(i)) != needle) continue;
        if (hitLine >= 0) return;          // ambiguous → report nothing
        hitLine = i + 1;                   // 1-based, matching read_region
        hitText = lines.at(i);
    }
    if (hitLine < 0) return;
    r.nearMissLine = hitLine;
    r.nearMissText = hitText;
    r.nearMissKind = QStringLiteral("whitespace");
}

}  // namespace

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
        findWhitespaceNearMiss(contents, oldStr, r);
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
