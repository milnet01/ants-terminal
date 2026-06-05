// ANTS-2022 — apply_edits pure in-memory edit helper. Qt6::Core-only.
// A whole-content substring replace per edit; the wrapper threads one
// edit's result into the next so two edits to one file compose, then
// writes atomically. See docs/specs/ANTS-2022.md.

#include "applyedits.h"

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

}  // namespace ApplyEdits
