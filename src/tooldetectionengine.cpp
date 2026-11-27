// ANTS-1286 — see header.

#include "tooldetectionengine.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#include <chrono>

namespace {

struct Entry {
    QString path;           // "" = not found
    qint64  probedAtMs = 0;  // steadyNowMs()
};

QMutex g_mutex;
QString g_pathHash;
QHash<QString, Entry> g_resolved;
qint64 g_missTtlMs = ToolDetectionEngine::kDefaultMissTtlMs;

QString hashCurrentPath() {
    const QByteArray p = qgetenv("PATH");
    return QString::fromLatin1(
        QCryptographicHash::hash(p, QCryptographicHash::Sha256)
            .toHex().left(16));
}

// A monotonic clock, so a wall-clock step back cannot hold a miss past its TTL.
qint64 steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void resetIfPathChangedLocked() {
    const QString h = hashCurrentPath();
    if (h != g_pathHash) {
        g_pathHash = h;
        g_resolved.clear();
    }
}

}  // namespace

namespace ToolDetectionEngine {

QString resolve(const QString &tool) {
    if (tool.isEmpty()) return {};
    QMutexLocker lock(&g_mutex);
    resetIfPathChangedLocked();
    const qint64 now = steadyNowMs();
    auto it = g_resolved.constFind(tool);
    if (it != g_resolved.constEnd()
        && (!it->path.isEmpty() || now - it->probedAtMs < g_missTtlMs)) {
        return it->path;
    }
    const QString p = QStandardPaths::findExecutable(tool);
    g_resolved.insert(tool, Entry{p, now});
    return p;
}

bool exists(const QString &tool) {
    return !resolve(tool).isEmpty();
}

void clearCache() {
    QMutexLocker lock(&g_mutex);
    g_pathHash.clear();
    g_resolved.clear();
}

int cacheSize() {
    QMutexLocker lock(&g_mutex);
    return g_resolved.size();
}

QString currentPathHash() {
    return hashCurrentPath();
}

void setMissTtlMs(qint64 ms) {
    QMutexLocker lock(&g_mutex);
    g_missTtlMs = ms < 0 ? kDefaultMissTtlMs : ms;
}

}  // namespace ToolDetectionEngine
