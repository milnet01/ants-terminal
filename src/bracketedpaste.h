#pragma once

#include <QByteArray>

// Strips the bracketed-paste start and end markers from a paste before
// TerminalWidget::performPaste wraps it, so pasted text cannot end the paste
// early and run the rest as typed input (CVE-2021-28848 class). Covers the
// 7-bit `ESC [`, the raw 8-bit CSI byte 0x9B and its UTF-8 form `C2 9B`.
// Contract: tests/features/bracketed_paste_sanitize/spec.md.
inline QByteArray stripBracketedPasteMarkers(QByteArray s) {
    static const char *const kMarkers[] = {
        "\x1B[200~", "\x1B[201~",
        "\xC2\x9B" "200~", "\xC2\x9B" "201~",  // UTF-8-encoded 8-bit CSI
        "\x9B" "200~", "\x9B" "201~",          // raw single-byte C1
    };
    // Repeat until nothing is removed: taking one marker out can join the
    // bytes around it into another, e.g. ESC[20 ESC[201~ 0~ (ANTS-6442).
    // Each pass that changes anything shrinks s, so the loop ends.
    for (;;) {
        const qsizetype before = s.size();
        for (const char *m : kMarkers) s.replace(m, "");
        if (s.size() == before) return s;
    }
}
