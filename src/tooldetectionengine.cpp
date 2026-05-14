// ANTS-1286 — see header.

#include "tooldetectionengine.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

namespace {

QMutex g_mutex;
QString g_pathHash;
QHash<QString, QString> g_resolved;   // tool → resolved path ("" = not found)

QString hashCurrentPath() {
    const QByteArray p = qgetenv("PATH");
    return QString::fromLatin1(
        QCryptographicHash::hash(p, QCryptographicHash::Sha256)
            .toHex().left(16));
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
    auto it = g_resolved.constFind(tool);
    if (it != g_resolved.constEnd()) return it.value();
    const QString p = QStandardPaths::findExecutable(tool);
    g_resolved.insert(tool, p);
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

}  // namespace ToolDetectionEngine
