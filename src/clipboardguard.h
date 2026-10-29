#pragma once

// ANTS-1014 — Clipboard-write redaction funnel.
//
// All `QApplication::clipboard()->setText(...)` sites must route
// through this header instead, declaring the source's trust shape.
// The 7th-audit memory flagged OSC 52 (PTY-driven clipboard writes)
// as an exfiltration vector that needs a centralised hygiene layer.
//
// `sanitize` is inline here and needs no QApplication, so the feature
// test drives it directly. The header still includes the Qt6::Gui
// header <QClipboard>, for writeText's QClipboard::Mode parameter.

#include <QClipboard>
#include <QString>

#include <cstdint>

namespace clipboardguard {

// Trust shape of the call site. Picks the sanitisation policy.
enum class Source : std::uint8_t {
    Trusted,          // user UI action — selection, context menu,
                      //   copy-last-output, URL quick-select copy
    UntrustedPty,     // OSC 52 from the shell process
    UntrustedPlugin,  // ants.clipboard.write from a Lua plugin
};

// Hard size cap on untrusted clipboard writes, measured in QString
// length units (UTF-16 code units / chars), NOT bytes — a loose
// defence-in-depth memory bound, not a precise byte limit (ANTS-2205).
// The OSC 52 parser layer already has its own cap; this is the
// backstop at the clipboard surface.
inline constexpr int kUntrustedMaxChars = 1 * 1024 * 1024;  // 1,048,576 chars

// Pure: returns a sanitised copy of `text` per the source's policy.
// - All sources: strip embedded `QChar(0)` (NUL).
// - UntrustedPty / UntrustedPlugin: truncate to kUntrustedMaxChars.
// - Trusted: no truncation (a 10 MiB context-menu copy is a
//   legitimate user flow).
inline QString sanitize(QString text, Source source) {
    text.remove(QChar(0));
    if (source != Source::Trusted && text.size() > kUntrustedMaxChars)
        text.truncate(kUntrustedMaxChars);
    return text;
}

// Wrapper around QApplication::clipboard()->setText().
void writeText(const QString &text,
               Source source,
               QClipboard::Mode mode = QClipboard::Clipboard);

}  // namespace clipboardguard
