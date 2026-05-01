// ANTS-1014 — Clipboard-write redaction funnel (impl).
//
// The thin wrapper that pulls in Qt6::Gui for the actual clipboard
// access. The interesting policy lives in clipboardguard.h's
// `sanitize` (pure), which the feature test drives directly.

#include "clipboardguard.h"

#include <QApplication>

namespace clipboardguard {

void writeText(const QString &text, Source source, QClipboard::Mode mode) {
    if (auto *cb = QApplication::clipboard())
        cb->setText(sanitize(text, source), mode);
}

}  // namespace clipboardguard
