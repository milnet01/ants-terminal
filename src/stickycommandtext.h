#pragma once

#include <QChar>
#include <QString>

#include <functional>

// ANTS-5027 — the command text the sticky header shows for an OSC 133
// prompt region spanning [startLine, endLine]: each line trimmed, joined
// by one space, trimmed, then cut to maxChars with an ellipsis. lineAt
// returns one global line's text. Empty result: draw no header.
//
// Program output places the region's end marker, so the span can reach
// the whole scrollback. Reading stops once the text is past what the
// header can show; every line adds at least its separator, so that also
// bounds how many lines are read.
inline QString stickyCommandText(int startLine, int endLine, int maxChars,
                                 const std::function<QString(int)> &lineAt) {
    QString cmdText;
    for (int gl = startLine; gl <= endLine; ++gl) {
        cmdText += lineAt(gl).trimmed();
        if (gl < endLine) cmdText += QLatin1Char(' ');
        if (maxChars > 0 && cmdText.length() > maxChars + 1) break;
    }
    cmdText = cmdText.trimmed();
    if (maxChars > 0 && cmdText.length() > maxChars)
        cmdText = cmdText.left(maxChars - 1) + QChar(0x2026);  // ellipsis
    return cmdText;
}
