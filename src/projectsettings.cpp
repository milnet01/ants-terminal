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
// ANTS-3369 retired kDominanceRatio: on a miss, suggest ALL first-party
// source subdirs (not a dominant-cover subset) so a low-count entry-point
// dir + a spread src-less layout are both covered, not silently dropped.

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

// ANTS-2161 / ANTS-3393 — single source of truth for "is this a top-level
// dir the source walk should neither descend nor index". Shared by
// op:detect (below) and codebase_index's walkSubtree so both prune the
// same vendored / build-output / Python-virtualenv trees; a committed venv
// no longer drowns a flat-root source_roots=["."] index.
bool isNoiseDir(const QString &name) {
    if (name.startsWith(QLatin1Char('.'))) return true;      // .git, .ants, .cache, .venv …
    if (name.startsWith(QLatin1String("build"))) return true; // build/, build-fast/, …
    // ANTS-3357 — discount well-known vendored / third-party trees so the
    // walk doesn't rank a bundled-dependency dir as a source_root (DOOM
    // bundles SDL2 + Vulkan-Headers under `mingw-deps/`, ~632 of 654 files).
    // A *-deps / *-prefix suffix is the strongest signal for cross-compile
    // dependency staging dirs; the exact names cover the conventional
    // vendoring + Python-virtualenv layouts. Belt-and-braces only — op:init
    // still accepts explicit source_roots to override.
    if (name.endsWith(QLatin1String("-deps")) ||
        name.endsWith(QLatin1String("-prefix"))) return true; // mingw-deps, x-prefix
    static const QStringList noise = {
        QStringLiteral("node_modules"), QStringLiteral("dist"),
        QStringLiteral("target"), QStringLiteral("vendor"), QStringLiteral("out"),
        QStringLiteral("third_party"), QStringLiteral("third-party"),
        QStringLiteral("deps"), QStringLiteral("external"), QStringLiteral("extern"),
        QStringLiteral("venv"), QStringLiteral("env"),  // ANTS-3393: bare Python virtualenvs (.venv caught by the dot rule)
        QStringLiteral("__pycache__")};                 // ANTS-3393: Python bytecode cache
    return noise.contains(name);
}

Suggestion detect(const QString &rootCanonical) {
    Suggestion s;
    if (rootCanonical.isEmpty()) return s;
    if (QFileInfo::exists(rootCanonical + QStringLiteral("/.ants/project.json"))) {
        // Already configured → no directory walk. Load the file to echo its
        // declared source_roots; never second-guess the layout (ANTS-3369).
        // An unparseable file → load() returns all-nullopt → wouldUseRoots
        // nullopt, reason still non-empty (INV-15); detect has no refusal.
        s.present = true;
        const Settings declared = load(rootCanonical);
        if (declared.sourceRoots) {
            s.wouldUseRoots = declared.sourceRoots;
            s.reason = QStringLiteral(
                "settings file present; %1 source_root(s) already declared (%2)")
                .arg(declared.sourceRoots->size())
                .arg(declared.sourceRoots->join(QStringLiteral(", ")));
        } else {
            s.reason = QStringLiteral(
                "settings file present; no source_roots declared "
                "(default src/+tests/ walk in use)");
        }
        return s;
    }

    QDir root(rootCanonical);
    int budget = kDetectFileCeiling;
    QList<QPair<QString, int>> dirCounts;       // top-level dir → admitted count
    QStringList excludedDirs;                    // ANTS-3369: skipped noise dirs (non-dot)
    const QFileInfoList dirs = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &fi : dirs) {
        const QString name = fi.fileName();
        if (isNoiseDir(name)) {
            // Surface the skipped vendored/build dirs (but not dot-dirs) so
            // a caller sees what was discounted — names only, no descent.
            if (!name.startsWith(QLatin1Char('.'))) excludedDirs << name;
            continue;
        }
        if (budget <= 0) continue;
        const int c = countSourceFiles(rootCanonical, name, budget);
        budget -= c;
        if (c > 0) dirCounts.append({name, c});
    }
    s.excluded = excludedDirs;

    // Files directly at the repo root (depth 0) — counted toward the total;
    // on a miss, rootLevel>0 drives a whole-root ["."] suggestion so they are
    // covered (ANTS-3390, INV-12/INV-17).
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

    if (total == 0) {                                           // empty repo
        s.reason = QStringLiteral("no source files found under the project root");
        return s;
    }
    if (s.defaultSourceCount >= kMissRatio * total) {           // default walk covers enough
        // No override needed — echo the default roots that hold source so
        // the zeros/empty reason don't read as a detection failure.
        QStringList defaults;
        if (srcCount   > 0) defaults << QStringLiteral("src");
        if (testsCount > 0) defaults << QStringLiteral("tests");
        if (!defaults.isEmpty()) s.wouldUseRoots = defaults;
        s.reason = QStringLiteral(
            "default src/+tests/ walk indexed %1 of %2 source files; "
            "no override needed").arg(s.defaultSourceCount).arg(total);
        return s;
    }

    // A miss with source loose AT the repo root (ANTS-3390) — suggest the
    // whole-root walk ["."], which walkSubtree noise-prunes exactly as it does
    // src/ (ANTS-3393). It covers BOTH the depth-0 entrypoint/orchestration
    // files (RetroArch's retroarch.c etc.) and every first-party subdir in one
    // entry. A subdirs-only suggestion can't reach root files, and source_roots
    // REPLACES the src/ default, so dropping them from the suggestion drops
    // them from the index. This is the rootLevel>0 half of the miss-space;
    // rootLevel==0 falls through to the subdir list (INV-14/INV-17).
    if (rootLevel > 0) {
        s.sourceRoots = QStringList{QStringLiteral(".")};
        s.reason = QStringLiteral(
            "default src/+tests/ walk indexed %1 of %2 source files; %3 file(s) "
            "sit loose at the repo root — suggesting the whole-root walk (\".\") "
            "so they are covered (vendored/build dirs auto-pruned)")
            .arg(s.defaultSourceCount).arg(total).arg(rootLevel);
        return s;
    }

    // A miss with all uncovered source in subdirs — suggest ALL first-party
    // source subdirs (every counted dir except the tests default), sorted
    // count desc / name asc (ANTS-3369: no dominant-cover gate, so a low-count
    // entry-point dir + a spread layout are both covered).
    QList<QPair<QString, int>> cands;
    for (const auto &p : dirCounts)
        if (p.first != QLatin1String("tests")) cands.append(p);
    std::sort(cands.begin(), cands.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (cands.isEmpty()) {
        // Defensive fallback (ANTS-3390): rootLevel==0 here — a miss WITH root
        // source returned ["."] above — and no first-party source subdir, so
        // all admitted source is under tests/. Near-unreachable via the miss
        // path (that layout has defaultSourceCount==total, so it is not a
        // miss), but keep a non-empty reason (INV-15) rather than fall through.
        s.reason = QStringLiteral(
            "default src/+tests/ walk indexed %1 of %2 source files; no "
            "first-party source subdir to suggest — declare source_roots "
            "manually").arg(s.defaultSourceCount).arg(total);
        return s;
    }
    QStringList chosen;
    int covered = 0;
    for (const auto &p : cands) { chosen << p.first; covered += p.second; }
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
