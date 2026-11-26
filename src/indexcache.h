// The on-disk cache codebase_index and docs_index share: where it lives, how
// it is read, how it is written back (ANTS-6592), and which cached paths a
// fresh walk finds stale (ANTS-6598). Each index keeps its own version check.
#pragma once

#include "sessionmemoryengine.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>

namespace IndexCache {

// <cache dir>/ants-terminal/<folder>/<hash of the root>.json
inline QString pathFor(const QString &folder, const QString &rootCanonical) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
         + QStringLiteral("/ants-terminal/") + folder + QLatin1Char('/')
         + SessionMemoryEngine::cwdHash(rootCanonical) + QStringLiteral(".json");
}

// The cached object; empty when the file is absent, unreadable or not a JSON
// object, which the caller treats as a cold build.
inline QJsonObject read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

// Write `obj` to `path` in one atomic replace. ANTS-5099, ANTS-5103 — a
// failed write or commit is logged under `verb`; ignored, an unwritable cache
// meant a silent cold build on every call.
inline void write(const QString &path, const QJsonObject &obj, const char *verb) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile sf(path);
    const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if (!sf.open(QIODevice::WriteOnly) || sf.write(body) != body.size()
        || !sf.commit())
        qWarning("%s: cache write to %s failed: %s", verb,
                 qUtf8Printable(path), qUtf8Printable(sf.errorString()));
}

// Fills `ss`'s added, changed (mtime differs) and removed lists: the
// `cached` entries, each with `path` and `mtimeMs`, against a fresh `walk`.
// ANTS-5099, ANTS-5103 — a capped cache holds a PREFIX of the walk: paths past
// its last entry were never cached, so they are neither added nor stat'ed, or
// every call would refresh and rewrite the cache. A removed prefix path still
// triggers the refresh that lets the tail shift in.
template <class Entry, class Stale>
void diffAgainstWalk(const QVector<Entry> &cached, bool cachedIsPrefix,
                     const QString &rootCanonical, const QStringList &walk,
                     Stale &ss) {
    QMap<QString, qint64> cachedMtime;
    for (const Entry &e : cached) cachedMtime.insert(e.path, e.mtimeMs);

    qsizetype prefixEnd = walk.size();
    if (cachedIsPrefix && !cached.isEmpty()) {
        const qsizetype last = walk.indexOf(cached.constLast().path);
        if (last >= 0) prefixEnd = last + 1;
    }

    QSet<QString> walked;
    for (qsizetype i = 0; i < walk.size(); ++i) {
        const QString &rel = walk.at(i);
        walked.insert(rel);
        if (i >= prefixEnd) continue;
        const auto it = cachedMtime.constFind(rel);
        if (it == cachedMtime.constEnd()) { ss.added << rel; continue; }
        const qint64 m = QFileInfo(rootCanonical + QLatin1Char('/') + rel)
                             .lastModified().toMSecsSinceEpoch();
        if (it.value() != m) ss.changed << rel;
    }
    for (auto it = cachedMtime.constBegin(); it != cachedMtime.constEnd(); ++it)
        if (!walked.contains(it.key())) ss.removed << it.key();
}

}  // namespace IndexCache
