#pragma once

// ANTS-6596 — the one JSON file each of BuildCache (build.json) and
// TestResCache (tests.json) keeps under .audit_cache/: where it lives, the
// owner-only write and the read. Header-only, so neither cache links
// ants_audit_lib, whose AuditCache uses the same folder (auditcache.h).

#include "secureio.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>

#include <optional>

namespace PrivateJsonCache {

inline QString dir(const QString &canonProject) {
    if (canonProject.isEmpty()) return {};
    return canonProject + QLatin1String("/.audit_cache");
}

// ANTS-5151 — these files quote the project's code (compiler output, test
// excerpts), so the temp file is private or nothing is written. Creates the
// folder. False leaves any earlier file as it was; `what` names the file in
// the warning writeFileAtomicOwnerOnly logs.
[[nodiscard]] inline bool write(const QString &path, const QJsonObject &obj,
                                const char *what) {
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    return writeFileAtomicOwnerOnly(
        path, path + QLatin1String(".tmp"),
        QJsonDocument(obj).toJson(QJsonDocument::Indented), what,
        TempPerms::Refuse);
}

// The file's JSON object, or nothing when it is missing, unreadable or not
// an object. With `maxBytes` >= 0, a file over it is refused after reading
// one byte past the cap, not the whole file (ANTS-6528).
inline std::optional<QJsonObject> read(const QString &path, qint64 maxBytes = -1) {
    if (path.isEmpty()) return std::nullopt;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray bytes = maxBytes < 0 ? f.readAll() : f.read(maxBytes + 1);
    if (maxBytes >= 0 && bytes.size() > maxBytes) return std::nullopt;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    return doc.object();
}

}  // namespace PrivateJsonCache
