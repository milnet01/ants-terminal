// ANTS-2160 — per-project settings file loader. See projectsettings.h +
// docs/specs/ANTS-2160.md.

#include "projectsettings.h"

#include "pathvalidation.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace ProjectSettings {

namespace {

// A declared relative path is valid iff it is a non-blank string and
// stays inside the canonical root. isInsideProject canonicalises the
// candidate, so a root-escape OR a non-existent path (canonicalisation
// miss) both return false → dropped (INV-4/INV-5/INV-8). Returns the
// trimmed relative path on success.
std::optional<QString> validEntry(const QString &rootCanonical,
                                  const QJsonValue &v) {
    if (!v.isString()) return std::nullopt;          // null / number / etc.
    const QString rel = v.toString().trimmed();
    if (rel.isEmpty()) return std::nullopt;          // empty / blank
    const QString candidate = rootCanonical + QLatin1Char('/') + rel;
    if (!PathValidation::isInsideProject(rootCanonical, candidate))
        return std::nullopt;
    return rel;
}

// A directory-array key (source_roots / test_roots): wrong-typed → nullopt;
// surviving entries kept; an array emptied by dropping → nullopt (INV-10).
std::optional<QStringList> parseDirArray(const QString &rootCanonical,
                                         const QJsonValue &v) {
    if (!v.isArray()) return std::nullopt;
    QStringList out;
    const QJsonArray arr = v.toArray();
    for (const QJsonValue &e : arr)
        if (auto ok = validEntry(rootCanonical, e)) out << *ok;
    if (out.isEmpty()) return std::nullopt;
    return out;
}

}  // namespace

Settings load(const QString &rootCanonical) {
    Settings s;
    if (rootCanonical.isEmpty()) return s;

    QFile f(rootCanonical + QStringLiteral("/.ants/project.json"));
    if (!f.open(QIODevice::ReadOnly)) return s;       // absent / unreadable
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return s;                                     // malformed / non-object

    const QJsonObject o = doc.object();
    s.sourceRoots = parseDirArray(rootCanonical, o.value(QStringLiteral("source_roots")));
    s.testRoots   = parseDirArray(rootCanonical, o.value(QStringLiteral("test_roots")));
    s.docsDir     = validEntry(rootCanonical, o.value(QStringLiteral("docs_dir")));
    s.roadmap     = validEntry(rootCanonical, o.value(QStringLiteral("roadmap")));
    s.changelog   = validEntry(rootCanonical, o.value(QStringLiteral("changelog")));
    s.specsDir    = validEntry(rootCanonical, o.value(QStringLiteral("specs_dir")));
    return s;
}

}  // namespace ProjectSettings
