// Writing a cell's codepoint as UTF-16 (ANTS-6615). TerminalWidget's
// lineText, lineColumns and accessibility text, and the scrollback exporter,
// encode through these, so a codepoint's units and their count follow one
// rule.
#pragma once

#include <QString>

#include <cstdint>

namespace Utf16 {

// UTF-16 units `cp` takes: two from U+10000 up.
inline int units(uint32_t cp) { return cp >= 0x10000 ? 2 : 1; }

// Writes `cp` at `out` and returns the position after it. No validation: a
// caller that may hold an invalid codepoint replaces it first.
inline char16_t *put(char16_t *out, uint32_t cp) {
    if (cp < 0x10000) {
        *out++ = static_cast<char16_t>(cp);
    } else {
        cp -= 0x10000;
        *out++ = static_cast<char16_t>(0xD800 + (cp >> 10));
        *out++ = static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    }
    return out;
}

inline void append(QString &text, uint32_t cp) {
    char16_t buf[2];
    text.append(reinterpret_cast<const QChar *>(buf), put(buf, cp) - buf);
}

}  // namespace Utf16
