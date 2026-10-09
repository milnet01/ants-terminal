#pragma once

// ANTS-5623 — a selection's text, assembled from its LAST line backwards.
// TerminalWidget::selectedText runs on the GUI thread, and a selection can
// span the whole scrollback (up to 1M lines). A caller that will keep only
// the tail (last_selection trims to its byte cap, keeping the end) passes
// `maxChars`, and assembly stops once that many characters are held, so the
// lines it would only throw away are never built. maxChars < 0 = no cap.

#include <QString>
#include <QStringList>

#include <algorithm>

struct SelectionTail {
    QString text;
    int linesSkipped = 0;   // leading lines never built because of the cap
};

// `lineAt(gl)` builds one line's text for global line `gl`, in [first, last].
// Lines are joined with '\n', exactly as an uncapped build joins them.
template <typename LineAt>
SelectionTail assembleSelectionTail(int first, int last, LineAt &&lineAt,
                                    qsizetype maxChars) {
    SelectionTail r;
    QStringList lines;
    qsizetype held = 0;
    for (int gl = last; gl >= first; --gl) {
        lines.append(lineAt(gl));
        held += lines.constLast().size() + 1;
        if (maxChars >= 0 && held >= maxChars && gl > first) {
            r.linesSkipped = gl - first;
            break;
        }
    }
    std::reverse(lines.begin(), lines.end());
    r.text = lines.join(QLatin1Char('\n'));
    return r;
}
