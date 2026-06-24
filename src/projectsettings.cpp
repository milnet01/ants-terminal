// ANTS-2160 — per-project settings file loader. See projectsettings.h +
// docs/specs/ANTS-2160.md.

#include "projectsettings.h"

#include "codebaseindex.h"
#include "pathvalidation.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>

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

// ----- ANTS-2161 — detector + writer ------------------------------------

namespace {

constexpr int    kDetectFileCeiling = 20000;  // walk safety cap (§5)
constexpr double kMissRatio         = 0.5;    // default walk must miss > this
constexpr double kDominanceRatio    = 0.9;    // suggested dirs must cover >= this

// Top-level dirs the detector never descends or suggests.
bool isNoiseDir(const QString &name) {
    if (name.startsWith(QLatin1Char('.'))) return true;      // .git, .ants, .cache, …
    if (name.startsWith(QLatin1String("build"))) return true; // build/, build-fast/, …
    static const QStringList noise = {
        QStringLiteral("node_modules"), QStringLiteral("dist"),
        QStringLiteral("target"), QStringLiteral("vendor"), QStringLiteral("out")};
    return noise.contains(name);
}

// Admitted-suffix files under <root>/<sub> (recursive), capped at `budget`.
int countSourceFiles(const QString &root, const QString &sub, int budget) {
    int n = 0;
    QDirIterator it(root + QLatin1Char('/') + sub,
                    QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext() && n < budget) {
        it.next();
        if (CodebaseIndex::isIndexableSuffix(it.fileInfo().suffix().toLower())) ++n;
    }
    return n;
}

bool validDirUnder(const QString &root, const QString &rel) {
    const QString abs = root + QLatin1Char('/') + rel;
    return PathValidation::isInsideProject(root, abs) && QFileInfo(abs).isDir();
}
bool validFileUnder(const QString &root, const QString &rel) {
    const QString abs = root + QLatin1Char('/') + rel;
    return PathValidation::isInsideProject(root, abs) && QFileInfo(abs).isFile();
}

}  // namespace

Suggestion detect(const QString &rootCanonical) {
    Suggestion s;
    if (rootCanonical.isEmpty()) return s;
    if (QFileInfo::exists(rootCanonical + QStringLiteral("/.ants/project.json"))) {
        s.present = true;                       // already configured → no walk
        return s;
    }

    QDir root(rootCanonical);
    int budget = kDetectFileCeiling;
    QList<QPair<QString, int>> dirCounts;       // top-level dir → admitted count
    const QFileInfoList dirs = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &fi : dirs) {
        if (budget <= 0) break;
        const QString name = fi.fileName();
        if (isNoiseDir(name)) continue;
        const int c = countSourceFiles(rootCanonical, name, budget);
        budget -= c;
        if (c > 0) dirCounts.append({name, c});
    }
    // Files directly at the repo root (depth 0) — counted toward the total
    // but never suggestable (subdirs only, INV-12).
    int rootLevel = 0;
    if (budget > 0) {
        const QFileInfoList files =
            root.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &fi : files)
            if (CodebaseIndex::isIndexableSuffix(fi.suffix().toLower())) ++rootLevel;
    }

    int total = rootLevel, srcCount = 0, testsCount = 0;
    for (const auto &p : dirCounts) {
        total += p.second;
        if (p.first == QLatin1String("src"))   srcCount   = p.second;
        if (p.first == QLatin1String("tests")) testsCount = p.second;
    }
    s.totalSourceCount   = total;
    s.defaultSourceCount = srcCount + testsCount;
    if (total == 0) return s;                                   // empty repo
    if (s.defaultSourceCount >= kMissRatio * total) return s;   // default walk covers enough

    // Candidate source dirs (exclude the tests default), sorted by count
    // desc, name asc for determinism; take until they cover kDominanceRatio.
    QList<QPair<QString, int>> cands;
    for (const auto &p : dirCounts)
        if (p.first != QLatin1String("tests")) cands.append(p);
    std::sort(cands.begin(), cands.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QStringList chosen;
    int covered = 0;
    for (const auto &p : cands) {
        chosen << p.first;
        covered += p.second;
        if (covered >= kDominanceRatio * total) break;
    }
    if (covered < kDominanceRatio * total) return s;            // no dominant subdir set (INV-14)

    s.sourceRoots = chosen;
    s.reason = QStringLiteral(
        "default src/+tests/ walk indexed %1 of %2 source files; %3 hold(s) %4")
        .arg(s.defaultSourceCount).arg(total)
        .arg(chosen.join(QStringLiteral(", "))).arg(covered);
    return s;
}

std::optional<QJsonObject> applyWrite(const QJsonObject &existing,
                                      const QJsonObject &changes,
                                      const QString &rootCanonical,
                                      QString *errCode, QString *errKey,
                                      QString *errVal) {
    const auto fail = [&](const QString &code, const QString &key,
                          const QString &val) -> std::optional<QJsonObject> {
        if (errCode) *errCode = code;
        if (errKey)  *errKey  = key;
        if (errVal)  *errVal  = val;
        return std::nullopt;
    };
    QJsonObject out = existing;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QString key = it.key();
        const QJsonValue v = it.value();
        if (v.isNull()) { out.remove(key); continue; }          // explicit clear (INV-8)

        const bool dirArray = key == QLatin1String("source_roots")
                           || key == QLatin1String("test_roots");
        const bool dir      = key == QLatin1String("docs_dir")
                           || key == QLatin1String("specs_dir");
        const bool file     = key == QLatin1String("roadmap")
                           || key == QLatin1String("changelog");

        if (dirArray) {
            if (!v.isArray())
                return fail(QStringLiteral("bad_args"), key, QStringLiteral("(not an array)"));
            const QJsonArray arr = v.toArray();
            if (arr.isEmpty())
                return fail(QStringLiteral("bad_args"), key, QStringLiteral("(empty array)"));
            for (const QJsonValue &e : arr) {
                if (!e.isString())
                    return fail(QStringLiteral("bad_args"), key, QStringLiteral("(non-string entry)"));
                const QString rel = e.toString().trimmed();
                if (rel.isEmpty() || !validDirUnder(rootCanonical, rel))
                    return fail(QStringLiteral("bad_path"), key, e.toString());
            }
            out[key] = arr;
        } else if (dir) {
            if (!v.isString())
                return fail(QStringLiteral("bad_args"), key, QStringLiteral("(not a string)"));
            const QString rel = v.toString().trimmed();
            if (rel.isEmpty() || !validDirUnder(rootCanonical, rel))
                return fail(QStringLiteral("bad_path"), key, v.toString());
            out[key] = rel;
        } else if (file) {
            if (!v.isString())
                return fail(QStringLiteral("bad_args"), key, QStringLiteral("(not a string)"));
            const QString rel = v.toString().trimmed();
            if (rel.isEmpty() || !validFileUnder(rootCanonical, rel))
                return fail(QStringLiteral("bad_path"), key, v.toString());
            out[key] = rel;
        } else {
            out[key] = v;                                       // unknown key — preserve (forward-compat)
        }
    }
    return out;
}

}  // namespace ProjectSettings
