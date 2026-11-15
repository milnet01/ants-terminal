// Cutting UTF-8 bytes to a byte budget without splitting a character
// (ANTS-6269). Used by git_state op:"log" and changelog_log op:"release".
#pragma once

#include <QByteArray>

namespace Utf8Cut {

// The longest prefix of `utf8` of at most `cap` bytes that ends on a character
// boundary. A byte of the form 10xxxxxx continues a character, so the cut
// backs off past those to the character's lead byte.
inline QByteArray prefix(const QByteArray &utf8, qsizetype cap) {
    if (cap >= utf8.size()) return utf8;
    if (cap <= 0) return {};
    qsizetype n = cap;
    while (n > 0 && (static_cast<unsigned char>(utf8.at(n)) & 0xC0) == 0x80) --n;
    return utf8.left(n);
}

}  // namespace Utf8Cut
