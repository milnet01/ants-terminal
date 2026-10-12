#include "claudetranscriptwatch.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>

namespace {
QPointer<ClaudeTranscriptWatch> &hubSlot() {
    static QPointer<ClaudeTranscriptWatch> hub;
    return hub;
}
} // namespace

ClaudeTranscriptWatch &ClaudeTranscriptWatch::instance() {
    QPointer<ClaudeTranscriptWatch> &hub = hubSlot();
    if (!hub) hub = new ClaudeTranscriptWatch(QCoreApplication::instance());
    return *hub;
}

ClaudeTranscriptWatch *ClaudeTranscriptWatch::existing() {
    return hubSlot().data();
}

ClaudeTranscriptWatch::ClaudeTranscriptWatch(QObject *parent) : QObject(parent) {
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &ClaudeTranscriptWatch::changed);
}

void ClaudeTranscriptWatch::acquire(const QString &path) {
    if (path.isEmpty()) return;
    ++m_refs[path];
    ensure(path);
}

void ClaudeTranscriptWatch::release(const QString &path) {
    const auto it = m_refs.find(path);
    if (it == m_refs.end()) return;
    if (--it.value() > 0) return;
    m_refs.erase(it);
    m_watcher.removePath(path);
}

bool ClaudeTranscriptWatch::ensure(const QString &path) {
    if (!m_refs.contains(path) || isWatching(path)) return false;
    if (!QFileInfo::exists(path)) return false;
    return m_watcher.addPath(path);
}

bool ClaudeTranscriptWatch::isWatching(const QString &path) const {
    return m_watcher.files().contains(path);
}

int ClaudeTranscriptWatch::subscribers(const QString &path) const {
    return m_refs.value(path, 0);
}
