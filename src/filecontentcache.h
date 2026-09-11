#pragma once

// ANTS-5056 — one file-content cache for the review engines (BriefDispatch,
// ColdEyesEngine, IndieReviewEngine, DebtSweepEngine). Each kept its own
// function-static copy, commented as single-threaded; since ANTS-2132 their
// verbs run on the MCP worker while the dialogs fill the same caches on the
// GUI thread. Keyed by (path, read mode, mtime); every access is locked, and
// the bodies held never exceed kByteBudget.

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QString>

namespace FileContentCache {

// Byte budget for cached bodies (UTF-16 bytes).
inline constexpr qint64 kByteBudget = qint64(32) * 1024 * 1024;

namespace detail {
struct State {
    QMutex mutex;
    QHash<QString, QPair<qint64, QString>> entries;
    qint64 bytes = 0;
};
inline State &state() {
    static State s;
    return s;
}
inline qint64 bodyBytes(const QString &s) { return s.size() * qint64(sizeof(QChar)); }
}  // namespace detail

// The file's contents decoded as UTF-8, or "" when it cannot be opened.
// `textMode` opens with QIODevice::Text (CRLF → LF), as BriefDispatch did.
inline QString slurpUtf8(const QString &absPath, bool textMode = false) {
    const QString key = (textMode ? QStringLiteral("t:") : QStringLiteral("b:")) + absPath;
    const qint64 mtime = QFileInfo(absPath).lastModified().toMSecsSinceEpoch();
    auto &st = detail::state();
    {
        QMutexLocker lock(&st.mutex);
        const auto it = st.entries.constFind(key);
        if (it != st.entries.constEnd() && mtime != 0 && it->first == mtime)
            return it->second;
    }
    // Read outside the lock, so a slow read does not stall the other thread.
    QFile f(absPath);
    QIODevice::OpenMode mode = QIODevice::ReadOnly;
    if (textMode) mode |= QIODevice::Text;
    if (!f.open(mode)) return {};
    const QString content = QString::fromUtf8(f.readAll());
    const qint64 size = detail::bodyBytes(content);

    QMutexLocker lock(&st.mutex);
    if (const auto old = st.entries.constFind(key); old != st.entries.constEnd()) {
        st.bytes -= detail::bodyBytes(old->second);
        st.entries.erase(old);
    }
    if (size > kByteBudget) return content;  // served, never held
    if (st.bytes + size > kByteBudget) {     // clear on overflow: a re-read is cheap
        st.entries.clear();
        st.bytes = 0;
    }
    st.entries.insert(key, {mtime, content});
    st.bytes += size;
    return content;
}

// Bytes of file content currently held.
inline qint64 cachedBytes() {
    auto &st = detail::state();
    QMutexLocker lock(&st.mutex);
    return st.bytes;
}

inline void clear() {
    auto &st = detail::state();
    QMutexLocker lock(&st.mutex);
    st.entries.clear();
    st.bytes = 0;
}

}  // namespace FileContentCache
