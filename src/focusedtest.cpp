// ANTS-1302 — `focused_test` resolution lib. See focusedtest.h and
// docs/specs/ANTS-1302.md.

#include "focusedtest.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace FocusedTest {

namespace {

// Append patterns into `into`, de-duplicated, order-preserved.
void addPatterns(QStringList &into, const QStringList &more) {
    for (const QString &p : more) {
        if (!p.isEmpty() && !into.contains(p)) into.append(p);
    }
}

// Limited glob support (ANTS-1775): a file matches an ignore entry if
//   (a) it ends with the entry as a suffix — a single leading "*" is
//       stripped so "*.md" -> ".md", and a bare ".md" / "config.cpp"
//       also matches by suffix; OR
//   (b) the entry names a path-segment prefix — "docs/" or "build"
//       matches "build/x.cpp" and the exact path "build", but NOT
//       "build_config.cpp". Pre-fix this used a raw startsWith(), so
//       "build" wrongly swallowed "build_config.cpp".
// This is NOT a full glob: interior "*"/"?"/character classes are not
// interpreted (only a single leading "*" on the suffix form).
bool matchesIgnore(const QString &file, const QStringList &globs) {
    for (const QString &g : globs) {
        if (g.isEmpty()) continue;
        QString suffix = g;
        if (suffix.startsWith(QLatin1Char('*'))) suffix = suffix.mid(1);
        if (!suffix.isEmpty() && file.endsWith(suffix)) return true;
        // Path-segment-anchored prefix: exact match, or match up to a
        // segment boundary ("/"). Honour an explicit trailing slash.
        if (file == g) return true;
        QString prefix = g;
        if (!prefix.endsWith(QLatin1Char('/')))
            prefix += QLatin1Char('/');
        if (file.startsWith(prefix)) return true;
    }
    return false;
}

bool isSourceLike(const QString &file) {
    static const QStringList kExts = {
        QStringLiteral(".cpp"), QStringLiteral(".cc"), QStringLiteral(".cxx"),
        QStringLiteral(".c"),   QStringLiteral(".h"),  QStringLiteral(".hpp"),
        QStringLiteral(".hh"),  QStringLiteral(".hxx"),
        QStringLiteral(".py"),  QStringLiteral(".lua"), QStringLiteral(".sh")};
    for (const QString &e : kExts) {
        if (file.endsWith(e)) return true;
    }
    return false;
}

}  // namespace

CoverageMap loadCoverageMap(const QString &rootCanonical) {
    CoverageMap cm;
    if (rootCanonical.isEmpty()) {
        cm.error = QStringLiteral("absent");
        return cm;
    }
    const QString path =
        rootCanonical + QStringLiteral("/tests/coverage-map.json");
    QFile f(path);
    if (!f.exists()) {
        cm.error = QStringLiteral("absent");
        return cm;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        cm.error = QStringLiteral("bad_json");
        return cm;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        cm.error = QStringLiteral("bad_json");
        return cm;
    }
    const QJsonObject root = doc.object();
    cm.schemaVersion =
        root.value(QStringLiteral("schema_version")).toInt(0);
    if (cm.schemaVersion != 1) {
        cm.error = QStringLiteral("bad_schema");
        return cm;
    }
    const QJsonObject mapObj =
        root.value(QStringLiteral("map")).toObject();
    for (auto it = mapObj.constBegin(); it != mapObj.constEnd(); ++it) {
        QStringList pats;
        for (const QJsonValue &v : it.value().toArray()) {
            const QString s = v.toString();
            if (!s.isEmpty()) pats.append(s);
        }
        cm.entries.insert(it.key(), pats);
    }
    for (const QJsonValue &v :
         root.value(QStringLiteral("default")).toArray()) {
        const QString s = v.toString();
        if (!s.isEmpty()) cm.defaultPatterns.append(s);
    }
    for (const QJsonValue &v :
         root.value(QStringLiteral("ignore")).toArray()) {
        const QString s = v.toString();
        if (!s.isEmpty()) cm.ignoreGlobs.append(s);
    }
    cm.valid = true;
    cm.error.clear();
    return cm;
}

QString heuristicPattern(const QString &changedFile) {
    if (!isSourceLike(changedFile)) return QString();
    QString base = changedFile;
    const int slash = base.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) base = base.mid(slash + 1);
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) base = base.left(dot);
    base = base.trimmed();
    if (base.isEmpty()) return QString();
    return QRegularExpression::escape(base);
}

QString buildCtestRegex(const QStringList &patterns) {
    if (patterns.isEmpty()) return QString();
    return QLatin1Char('(') + patterns.join(QLatin1Char('|')) +
           QLatin1Char(')');
}

Resolution resolve(const QStringList &changedFiles,
                   const CoverageMap &map) {
    Resolution r;
    if (changedFiles.isEmpty()) {
        r.selection = Selection::Full;
        r.reason = QStringLiteral("no changed files");
        return r;
    }

    if (!map.valid) {
        // Heuristic mode.
        for (const QString &f : changedFiles) {
            const QString p = heuristicPattern(f);
            if (p.isEmpty()) {
                r.ignoredFiles.append(f);
            } else {
                addPatterns(r.patterns, {p});
                r.mappedFiles.append(f);
            }
        }
        if (!r.patterns.isEmpty()) {
            r.selection = Selection::Heuristic;
            r.reason = QStringLiteral(
                "no coverage map; heuristic matched %1 source file(s)")
                .arg(r.mappedFiles.size());
        } else {
            r.selection = Selection::Full;
            r.reason = QStringLiteral(
                "no coverage map; heuristic matched no source files");
        }
        return r;
    }

    // Map mode.
    QString firstUnmapped;
    for (const QString &f : changedFiles) {
        if (matchesIgnore(f, map.ignoreGlobs)) {
            r.ignoredFiles.append(f);
            continue;
        }
        const auto it = map.entries.constFind(f);
        if (it != map.entries.constEnd()) {
            addPatterns(r.patterns, it.value());
            r.mappedFiles.append(f);
        } else if (!map.defaultPatterns.isEmpty()) {
            addPatterns(r.patterns, map.defaultPatterns);
            r.mappedFiles.append(f);
        } else {
            r.unmappedFiles.append(f);
            if (firstUnmapped.isEmpty()) firstUnmapped = f;
        }
    }

    if (!r.unmappedFiles.isEmpty()) {
        r.selection = Selection::Full;
        r.reason = QStringLiteral(
            "unmapped file %1 (running full suite)").arg(firstUnmapped);
        return r;
    }
    if (!r.patterns.isEmpty()) {
        r.selection = Selection::Map;
        r.reason = QStringLiteral("%1 file(s) mapped to %2 pattern(s)")
                       .arg(r.mappedFiles.size())
                       .arg(r.patterns.size());
        return r;
    }
    r.selection = Selection::Full;
    r.reason = QStringLiteral(
        "only test-irrelevant files changed; running full suite");
    return r;
}

}  // namespace FocusedTest
