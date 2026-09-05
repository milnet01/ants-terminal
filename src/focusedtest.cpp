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

// ANTS-2008 — a build-system change can alter how ANY target compiles or
// links, so it must run the whole suite rather than the subset of the source
// files that happened to change alongside it.
bool isBuildSystemFile(const QString &file) {
    const int slash = file.lastIndexOf(QLatin1Char('/'));
    const QString name = (slash >= 0) ? file.mid(slash + 1) : file;
    return name == QLatin1String("CMakeLists.txt")
        || name == QLatin1String("CMakePresets.json")
        || name.endsWith(QLatin1String(".cmake"))
        || name.endsWith(QLatin1String(".cmake.in"));
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
        // ANTS-2008 — the file exists but couldn't be opened (perms / I/O):
        // distinct from malformed JSON.
        cm.error = QStringLiteral("read_failed");
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
    // ANTS-2008 — validate every map pattern as a regex. An unvalidated typo
    // (e.g. an unbalanced paren) flows straight into the ctest `-R` regex and
    // turns that focused_test run into a hard ctest failure for every file
    // pointing at the bad entry. Fail loudly to `bad_pattern` instead — the
    // caller then falls back to the (escaped, always-valid) heuristic.
    auto invalidPattern = [](const QString &p) {
        return !QRegularExpression(p).isValid();
    };
    const QJsonObject mapObj =
        root.value(QStringLiteral("map")).toObject();
    for (auto it = mapObj.constBegin(); it != mapObj.constEnd(); ++it) {
        // ANTS-4460 — a value that is not an array (the natural mistake being a
        // bare string, `"src/foo.cpp": "FooTest"`) gave an EMPTY toArray(): the
        // key was inserted carrying no patterns, so the file counted as MAPPED
        // and contributed nothing to the ctest `-R` regex. The run then
        // selected no tests and reported success — a false green from a typo.
        // Refuse the shape, as the invalid-pattern check beside it does.
        if (!it.value().isArray()) {
            cm.error = QStringLiteral("bad_schema");
            return cm;
        }
        QStringList pats;
        for (const QJsonValue &v : it.value().toArray()) {
            const QString s = v.toString();
            if (s.isEmpty()) continue;
            if (invalidPattern(s)) {
                cm.error = QStringLiteral("bad_pattern");
                return cm;
            }
            pats.append(s);
        }
        cm.entries.insert(it.key(), pats);
    }
    // Same shape guard as the map above, but only when the key is PRESENT: an
    // absent `default` is legitimate and must stay an empty pattern list.
    if (root.contains(QStringLiteral("default")) &&
        !root.value(QStringLiteral("default")).isArray()) {
        cm.error = QStringLiteral("bad_schema");
        return cm;
    }
    for (const QJsonValue &v :
         root.value(QStringLiteral("default")).toArray()) {
        const QString s = v.toString();
        if (s.isEmpty()) continue;
        if (invalidPattern(s)) {
            cm.error = QStringLiteral("bad_pattern");
            return cm;
        }
        cm.defaultPatterns.append(s);
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

    // ANTS-2008 — a build-system change forces the full suite (it can affect
    // any target). Checked before map/heuristic resolution so it can't be
    // demoted to a subset by landing in ignoredFiles.
    for (const QString &f : changedFiles) {
        if (isBuildSystemFile(f)) {
            r.selection = Selection::Full;
            r.reason =
                QStringLiteral("build-system change (%1) forces full suite")
                    .arg(f);
            return r;
        }
    }

    if (!map.valid) {
        // Heuristic mode.
        QString firstUnmapped;
        for (const QString &f : changedFiles) {
            const QString p = heuristicPattern(f);
            if (p.isEmpty()) {
                // ANTS-2119 M1 — a file with no usable basename stem (Makefile,
                // .json, .bin, a dotfile — anything not source-like) is
                // UNMAPPABLE, not ignore-glob-matched: heuristic mode consults
                // no ignore globs, so labelling it ignoredFiles is wrong AND
                // masks a coverage gap. Bucket it as unmappedFiles.
                r.unmappedFiles.append(f);
                if (firstUnmapped.isEmpty()) firstUnmapped = f;
            } else {
                addPatterns(r.patterns, {p});
                r.mappedFiles.append(f);
            }
        }
        // ANTS-2119 M1 — like map mode's INV-4, an unmappable file forces Full
        // so heuristic mode stays as conservative as map mode ("err toward more
        // tests"): we can't know which tests an unmappable file affects.
        if (!r.unmappedFiles.isEmpty()) {
            r.selection = Selection::Full;
            r.reason = QStringLiteral(
                "no coverage map; unmappable file %1 (running full suite)")
                .arg(firstUnmapped);
        } else if (!r.patterns.isEmpty()) {
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
