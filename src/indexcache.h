// The on-disk cache codebase_index and docs_index share: where it lives, how
// it is read, and how it is written back (ANTS-6592). Each index keeps its
// own version check and stale walk, which differ by what it indexes.
#pragma once

#include "sessionmemoryengine.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>

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

}  // namespace IndexCache
