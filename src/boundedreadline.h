// ANTS-5103 — read one line without holding all of it.
//
// QIODevice::readLine() with no bound allocates the whole line, so a single
// multi-megabyte line (a minified bundle, a generated table) was held in full
// by scanners whose specs promise a per-line buffer (ANTS-1249, ANTS-1303).
// This keeps at most `keep` bytes and consumes the rest of the line in
// bounded chunks, reporting the line's true length so byte offsets stay
// exact.

#pragma once

#include <QByteArray>
#include <QIODevice>

namespace BoundedReadLine {

// Returns up to `keep` bytes of the next line (with its '\n' when that fits).
// `*fullBytes`, when given, is the line's real length including the newline.
inline QByteArray read(QIODevice &dev, qint64 keep, qint64 *fullBytes = nullptr) {
    QByteArray line = dev.readLine(keep);
    qint64 total = line.size();
    if (!line.endsWith('\n')) {
        constexpr qint64 kDrainChunk = qint64(64) * 1024;
        while (!dev.atEnd()) {
            const QByteArray more = dev.readLine(kDrainChunk);
            total += more.size();
            if (more.endsWith('\n')) break;
        }
    }
    if (fullBytes) *fullBytes = total;
    return line;
}

// ANTS-6367 — dev.readLine(maxSize), returned at the line's own size.
// Qt's overload allocates maxSize bytes on every call and the result keeps
// that capacity, so a 7-byte line read at a 1 MiB cap holds 1 MiB. Under
// ASan, freeing that block once per line made a 100,000-line read take
// 23 s instead of 0.1 s. This reads into `scratch`, which the caller keeps
// across calls, and copies out only the bytes read. Like Qt's overload it
// returns at most maxSize - 1 bytes.
inline QByteArray readTight(QIODevice &dev, qint64 maxSize, QByteArray &scratch) {
    if (scratch.size() < maxSize) scratch.resize(maxSize);
    const qint64 n = dev.readLine(scratch.data(), maxSize);
    return n > 0 ? QByteArray(scratch.constData(), n) : QByteArray();
}

}  // namespace BoundedReadLine
