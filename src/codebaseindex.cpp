// ANTS-1637 — codebase_index pure helper. Qt6::Core-only, FS-reading, no
// widgets / no subprocess (mirrors findsources.cpp). See docs/specs/ANTS-1637.md.

#include "codebaseindex.h"

#include "fileoutline.h"
#include "subsystemmap.h"
#include "sessionmemoryengine.h"
#include "projectsettings.h"   // ANTS-2160 — source_roots / test_roots override

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace CodebaseIndex {

// Indexed-suffix set (§ 2.2): the walk admits only these. Exported
// (ANTS-2161) so the project_settings layout detector counts source files
// by the same rule — declared in codebaseindex.h.
bool isIndexableSuffix(const QString &suffixLower) {
    // ANTS-2148 — admit the full C/C++ family (incl. `.c`, `.cxx`, `.hxx`),
    // not just C++-only extensions. A C-only project (DOOM: 65 `.c` files)
    // was yielding file_count:0 because `.c` was absent here AND from
    // FileOutline::pickModeByExt; both now map the C family to the C++
    // outline regexes (C and C++ share enough surface syntax). Kept in
    // step with SymbolQuery::langForExt so count → outline → symbol query
    // cover the same files.
    // ANTS-2150 — also admit the brace family (Rust/Go/JS/TS/Java/C#/Kotlin/
    // Swift/Scala/PHP) + Ruby (`.rb`), kept in step with
    // FileOutline::genericLangName + SymbolQuery::langForExt so
    // count → outline → symbol query agree. (The candidates() walk still only
    // descends src/ + tests/; a non-src layout is a separate follow-on.)
    return suffixLower == QLatin1String("cpp") || suffixLower == QLatin1String("cc")
        || suffixLower == QLatin1String("cxx") || suffixLower == QLatin1String("c")
        || suffixLower == QLatin1String("h")   || suffixLower == QLatin1String("hpp")
        || suffixLower == QLatin1String("hh")  || suffixLower == QLatin1String("hxx")
        || suffixLower == QLatin1String("py")
        || suffixLower == QLatin1String("rs")  || suffixLower == QLatin1String("go")
        || suffixLower == QLatin1String("js")  || suffixLower == QLatin1String("jsx")
        || suffixLower == QLatin1String("mjs") || suffixLower == QLatin1String("cjs")
        || suffixLower == QLatin1String("ts")  || suffixLower == QLatin1String("tsx")
        || suffixLower == QLatin1String("java")|| suffixLower == QLatin1String("cs")
        || suffixLower == QLatin1String("kt")  || suffixLower == QLatin1String("kts")
        || suffixLower == QLatin1String("swift")|| suffixLower == QLatin1String("scala")
        || suffixLower == QLatin1String("sc")  || suffixLower == QLatin1String("php")
        || suffixLower == QLatin1String("rb")
    // ANTS-4096 — GLSL / Vulkan shader stages. find_definition (ANTS-3558)
    // and file_outline (ANTS-3800) have both admitted them since; this gate
    // was the one left behind, so a shader-heavy project's hand-written
    // sources were invisible to codebase_index AND to the indie_review
    // computed partition, which walks by this predicate. Delegated rather
    // than re-listed, per the in-step rule the comment above states.
        || FileOutline::isGlslExt(suffixLower);
}

namespace {

QString roleFor(const QString &relPath, const QString &basename) {
    const QString lower = basename.toLower();
    if (lower.endsWith(QLatin1String(".h")) || lower.endsWith(QLatin1String(".hpp"))
        || lower.endsWith(QLatin1String(".hh")))
        return QStringLiteral("header");
    if (relPath.startsWith(QLatin1String("tests/"))
        || basename.startsWith(QLatin1String("test_")))
        return QStringLiteral("test");
    return QStringLiteral("impl");
}

// Lane for one file. (1) longest module-map lane name the basename starts with
// (the live `subsystem op=files` src/<lane>* prefix rule); (2) else the
// basename stem when it ends with a family word (groups testauditengine.{cpp,h}
// — the ANTS-1636 gap); (3) else "".
QString laneFor(const QString &basename, const QStringList &laneNames) {
    QString best;
    for (const QString &n : laneNames)
        if (!n.isEmpty() && basename.startsWith(n) && n.size() > best.size())
            best = n;
    if (!best.isEmpty()) return best;

    static const QRegularExpression rx(QStringLiteral(
        "^[a-z0-9]+(?:engine|dialog|widget|cache|helper)\\.(?:cpp|cc|h|hpp|hh)$"));
    if (rx.match(basename).hasMatch()) {
        const int dot = basename.lastIndexOf(QLatin1Char('.'));
        return dot > 0 ? basename.left(dot) : basename;
    }
    return QString();
}

// Lane names from the canonical map source (docs/subsystems.md|CLAUDE.md). Read
// + parsed fresh (not the SubsystemMap static cache) so a refresh in the same
// process sees an edited fixture.
QStringList laneNamesFor(const QString &rootCanonical) {
    const QString src = SubsystemMap::resolveSource(
        rootCanonical + QStringLiteral("/CLAUDE.md"));
    QFile f(src);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QString content = QString::fromUtf8(f.readAll());
    QStringList names;
    for (const SubsystemMap::Lane &l : SubsystemMap::parse(content))
        if (!l.name.isEmpty()) names << l.name;
    return names;
}

// Deterministic candidate list: src/ (sorted) then tests/ (sorted), relative
// paths, admitted suffixes only.
QStringList walkSubtree(const QString &rootCanonical, const QString &sub) {
    QStringList out;
    const QString base = rootCanonical + QLatin1Char('/') + sub;
    if (!QDir(base).exists()) return out;
    QDirIterator it(base, QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    const int prefix = rootCanonical.size() + 1;  // strip "<root>/"
    while (it.hasNext()) {
        it.next();
        if (!isIndexableSuffix(it.fileInfo().suffix().toLower())) continue;
        QString rel = it.filePath().mid(prefix);
        // ANTS-3390 — a "." source_root (ANTS-3393 flat-root layouts, and the
        // ANTS-3390 op:detect suggestion for repo-root source) makes the walk
        // base "<root>/.", so QDirIterator yields "./"-prefixed paths
        // ("./retroarch.c"). Strip the single leading "./" so keys match the
        // src/-relative convention: findFile (the file_path lookup) is an exact
        // fe.path==rel match with no normalisation, and roleFor's tests/-prefix
        // detection would miss a "./tests/…" path. Only the base carries the
        // trailing ".", so the artifact is always exactly at the front.
        if (rel.startsWith(QLatin1String("./"))) rel = rel.mid(2);
        // ANTS-3393 — prune vendored / build-output / virtualenv trees even
        // under an explicit source_roots=["."]: a committed venv/ or
        // node_modules/ otherwise drowns the index (2350 vendored files vs
        // ~10 real ones on a flat-root Python project). isNoiseDir is the
        // same predicate op:detect uses, checked per directory component
        // (a "." / ".." segment from a "." root is not a real dir → skipped).
        bool noise = false;
        const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i + 1 < parts.size() && !noise; ++i) {
            const QString &seg = parts.at(i);
            if (seg == QLatin1String(".") || seg == QLatin1String("..")) continue;
            if (ProjectSettings::isNoiseDir(seg)) noise = true;
        }
        if (noise) continue;
        out << rel;
    }
    out.sort();
    return out;
}

QStringList candidates(const QString &rootCanonical) {
    // ANTS-2160 — honour .ants/project.json source_roots / test_roots when
    // present; else the src/ + tests/ default. Each declared root must be an
    // existing directory (a file-typed or vanished entry is skipped; if that
    // empties a key it falls back to its default — INV-5/INV-10). The load is
    // inside candidates() so every call-site (build / staleFiles / refresh)
    // is settings-aware (INV-12). removeDuplicates() collapses overlapping
    // nested declared roots (INV-9); for the disjoint src/tests default it is
    // a no-op, so absent-settings output is byte-identical (INV-1).
    const ProjectSettings::Settings s = ProjectSettings::load(rootCanonical);
    const auto resolve = [&](const std::optional<QStringList> &declared,
                             const QString &def) -> QStringList {
        if (!declared) return {def};
        QStringList dirs;
        for (const QString &r : *declared)
            if (QDir(rootCanonical + QLatin1Char('/') + r).exists()) dirs << r;
        return dirs.isEmpty() ? QStringList{def} : dirs;   // all dropped → default
    };
    QStringList out;
    for (const QString &r : resolve(s.sourceRoots, QStringLiteral("src")))
        out += walkSubtree(rootCanonical, r);
    for (const QString &r : resolve(s.testRoots, QStringLiteral("tests")))
        out += walkSubtree(rootCanonical, r);
    out.removeDuplicates();
    return out;
}

FileEntry outlineFile(const QString &rootCanonical, const QString &rel,
                      const QStringList &laneNames) {
    FileEntry fe;
    fe.path = rel;
    const QString abs = rootCanonical + QLatin1Char('/') + rel;
    const QFileInfo fi(abs);
    fe.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
    const QString base = fi.fileName();
    fe.role = roleFor(rel, base);
    fe.lane = laneFor(base, laneNames);

    const QJsonObject ol = FileOutline::compute(
        abs, FileOutline::Mode::Auto, /*includeDocComment=*/false,
        /*maxSymbols=*/1000);
    fe.language = ol.value(QStringLiteral("language")).toString();
    fe.lines = ol.value(QStringLiteral("total_lines")).toInt();
    const QJsonArray syms = ol.value(QStringLiteral("symbols")).toArray();
    fe.symbols.reserve(syms.size());
    for (const QJsonValue &v : syms) {
        const QJsonObject o = v.toObject();
        Symbol s;
        s.name = o.value(QStringLiteral("name")).toString();
        s.line = o.value(QStringLiteral("line")).toInt();
        s.kind = o.value(QStringLiteral("kind")).toString();
        fe.symbols << s;
    }
    return fe;
}

// Rough serialised-byte estimate for the incremental cache ceiling (INV-15).
qint64 estimateEntryBytes(const FileEntry &fe) {
    qint64 n = fe.path.size() + fe.language.size() + fe.role.size()
             + fe.lane.size() + 80;
    for (const Symbol &s : fe.symbols) n += s.name.size() + s.kind.size() + 40;
    return n;
}

void rebuildLaneToFiles(Index &idx) {
    idx.laneToFiles.clear();
    for (const FileEntry &fe : idx.files)
        if (!fe.lane.isEmpty()) idx.laneToFiles[fe.lane] << fe.path;
    for (auto it = idx.laneToFiles.begin(); it != idx.laneToFiles.end(); ++it)
        it.value().sort();
}

const FileEntry *findFile(const Index &idx, const QString &rel) {
    for (const FileEntry &fe : idx.files)
        if (fe.path == rel) return &fe;
    return nullptr;
}

QJsonObject symbolJson(const Symbol &s) {
    QJsonObject o;
    o[QStringLiteral("name")] = s.name;
    o[QStringLiteral("line")] = s.line;
    o[QStringLiteral("kind")] = s.kind;
    return o;
}

QJsonArray symbolsJson(const QVector<Symbol> &syms) {
    QJsonArray a;
    for (const Symbol &s : syms) a.append(symbolJson(s));
    return a;
}

QJsonObject fileEntryJson(const FileEntry &fe, bool withLane) {
    QJsonObject o;
    o[QStringLiteral("path")]     = fe.path;
    o[QStringLiteral("role")]     = fe.role;
    o[QStringLiteral("language")] = fe.language;
    o[QStringLiteral("lines")]    = fe.lines;
    if (withLane) o[QStringLiteral("lane")] = fe.lane;
    o[QStringLiteral("symbols")]  = symbolsJson(fe.symbols);
    return o;
}

}  // namespace

qint64 mapSourceMtimeMs(const QString &rootCanonical) {
    const QString src = SubsystemMap::resolveSource(
        rootCanonical + QStringLiteral("/CLAUDE.md"));
    const QFileInfo fi(src);
    return fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
}

Index build(const QString &rootCanonical, qint64 generatedAtMs,
            const Options &opts) {
    Index idx;
    idx.version          = kIndexVersion;
    idx.rootCanonical    = rootCanonical;
    idx.generatedAtMs    = generatedAtMs;
    idx.mapSourceMtimeMs = mapSourceMtimeMs(rootCanonical);

    const QStringList laneNames = laneNamesFor(rootCanonical);
    const QStringList cands     = candidates(rootCanonical);

    qint64 budget = 0;
    for (const QString &rel : cands) {
        if (idx.files.size() >= opts.maxIndexFiles) { idx.filesTruncated = true; break; }
        FileEntry fe = outlineFile(rootCanonical, rel, laneNames);
        const qint64 est = estimateEntryBytes(fe);
        if (!idx.files.isEmpty() && budget + est > opts.maxCacheBytes) {
            idx.filesTruncated = true; break;
        }
        budget += est;
        idx.files << fe;
    }
    rebuildLaneToFiles(idx);
    return idx;
}

// ANTS-2198 — internal worker accepting a precomputed candidate list, so the
// hot serve()->refresh() path walks candidates() (which loads
// .ants/project.json) exactly once instead of 2-3x, and runs the per-file
// stat pass once instead of twice. The public staleFiles/refresh below pass a
// fresh candidates() for standalone (test) callers.
static StaleSet staleFilesWith(const Index &prev, const QString &rootCanonical,
                               qint64 currentMapSourceMtimeMs,
                               const QStringList &cands) {
    StaleSet ss;
    ss.mapSourceChanged = currentMapSourceMtimeMs != prev.mapSourceMtimeMs;

    QSet<QString> prevPaths;
    QMap<QString, qint64> prevMtime;
    for (const FileEntry &fe : prev.files) {
        prevPaths.insert(fe.path);
        prevMtime.insert(fe.path, fe.mtimeMs);
    }

    QSet<QString> curSet;
    for (const QString &rel : cands) {
        curSet.insert(rel);
        const qint64 m = QFileInfo(rootCanonical + QLatin1Char('/') + rel)
                             .lastModified().toMSecsSinceEpoch();
        if (!prevPaths.contains(rel)) ss.added << rel;
        else if (prevMtime.value(rel) != m) ss.changed << rel;
    }
    for (const QString &p : prevPaths)
        if (!curSet.contains(p)) ss.removed << p;
    return ss;
}

StaleSet staleFiles(const Index &prev, const QString &rootCanonical,
                    qint64 currentMapSourceMtimeMs) {
    return staleFilesWith(prev, rootCanonical, currentMapSourceMtimeMs,
                          candidates(rootCanonical));
}

// ANTS-2198 — internal worker taking the precomputed StaleSet + candidate
// list so the serve() hot path does not recompute either.
static Index refreshWith(const Index &prev, const QString &rootCanonical,
                         qint64 generatedAtMs, qint64 currentMapSourceMtimeMs,
                         const Options &opts, int *refreshedOut,
                         const StaleSet &ss, const QStringList &cands) {
    QSet<QString> reoutline;
    for (const QString &p : ss.changed) reoutline.insert(p);
    for (const QString &p : ss.added)   reoutline.insert(p);

    QMap<QString, FileEntry> byPath;
    for (const FileEntry &fe : prev.files) byPath.insert(fe.path, fe);

    const QStringList laneNames = laneNamesFor(rootCanonical);

    Index idx;
    idx.version          = kIndexVersion;
    idx.rootCanonical    = rootCanonical;
    idx.generatedAtMs    = generatedAtMs;
    idx.mapSourceMtimeMs = currentMapSourceMtimeMs;

    qint64 budget = 0;
    for (const QString &rel : cands) {
        if (idx.files.size() >= opts.maxIndexFiles) { idx.filesTruncated = true; break; }
        FileEntry fe;
        if (reoutline.contains(rel) || !byPath.contains(rel)) {
            fe = outlineFile(rootCanonical, rel, laneNames);
        } else {
            fe = byPath.value(rel);
            if (ss.mapSourceChanged)  // re-derive lane only; keep symbols
                fe.lane = laneFor(QFileInfo(rel).fileName(), laneNames);
        }
        const qint64 est = estimateEntryBytes(fe);
        if (!idx.files.isEmpty() && budget + est > opts.maxCacheBytes) {
            idx.filesTruncated = true; break;
        }
        budget += est;
        idx.files << fe;
    }
    rebuildLaneToFiles(idx);
    if (refreshedOut) *refreshedOut = ss.changed.size() + ss.added.size();
    return idx;
}

Index refresh(const Index &prev, const QString &rootCanonical,
              qint64 generatedAtMs, qint64 currentMapSourceMtimeMs,
              const Options &opts, int *refreshedOut) {
    // Standalone (test) entry: compute the candidate list + stale set once,
    // then delegate. The hot serve() path computes these once itself and calls
    // refreshWith directly, so candidates() is walked once per serve (ANTS-2198).
    const QStringList cands = candidates(rootCanonical);
    const StaleSet ss =
        staleFilesWith(prev, rootCanonical, currentMapSourceMtimeMs, cands);
    return refreshWith(prev, rootCanonical, generatedAtMs,
                       currentMapSourceMtimeMs, opts, refreshedOut, ss, cands);
}

QJsonObject query(const Index &idx, const QueryParams &params,
                  int refreshedFiles, const QString &cachePath,
                  const Options &opts) {
    const int n = (!params.symbol.isEmpty() ? 1 : 0)
                + (!params.lane.isEmpty()   ? 1 : 0)
                + (!params.filePath.isEmpty() ? 1 : 0);
    if (n >= 2) {
        QJsonObject e;
        e[QStringLiteral("ok")]    = false;
        e[QStringLiteral("code")]  = QStringLiteral("bad_args");
        e[QStringLiteral("error")] = QStringLiteral(
            "codebase_index: at most one of {symbol, lane, file_path}");
        return e;
    }

    QJsonObject env;
    env[QStringLiteral("ok")]              = true;
    env[QStringLiteral("generated_at_ms")] = idx.generatedAtMs;
    env[QStringLiteral("file_count")]      = idx.files.size();
    env[QStringLiteral("refreshed_files")] = refreshedFiles;
    env[QStringLiteral("cache_path")]      = cachePath;
    env[QStringLiteral("files_truncated")] = idx.filesTruncated;
    bool symbolsTruncated = false;

    if (n == 0) {
        // Summary.
        QMap<QString, int> laneCounts, langCounts, roleCounts;
        for (const FileEntry &fe : idx.files) {
            if (!fe.lane.isEmpty()) laneCounts[fe.lane]++;
            langCounts[fe.language.isEmpty() ? QStringLiteral("unknown") : fe.language]++;
            roleCounts[fe.role]++;
        }
        // ANTS-3468 — opt-in compact lane→source-file digest. The counts-only
        // summary couldn't answer "where does subsystem X live", so the
        // session_orient bundle's map still forced a grep. When laneFiles is
        // set, each lane gains a `source_files` array of its NON-test paths
        // (test files are the bulk and low-signal for "where is the code");
        // symbol-level "where is X" stays a standalone codebase_index symbol=
        // call, too big for an always-on first call. Deterministic (sorted
        // lanes + sorted paths) so it does not perturb session_orient's 304
        // ETag; globally capped so a huge tree can't bloat the bundle.
        QMap<QString, QStringList> laneSource;   // lane → sorted non-test paths
        if (params.laneFiles) {
            for (const FileEntry &fe : idx.files)
                if (!fe.lane.isEmpty() && fe.role != QLatin1String("test"))
                    laneSource[fe.lane] << fe.path;
            for (auto it = laneSource.begin(); it != laneSource.end(); ++it)
                it.value().sort();
        }
        int  digestEmitted   = 0;
        bool digestTruncated = false;

        QJsonArray lanes;
        for (auto it = laneCounts.cbegin(); it != laneCounts.cend(); ++it) {
            QJsonObject l;
            l[QStringLiteral("lane")]       = it.key();
            l[QStringLiteral("file_count")] = it.value();
            if (params.laneFiles) {
                QJsonArray files;
                for (const QString &p : laneSource.value(it.key())) {
                    if (digestEmitted >= opts.maxLaneDigestFiles) {
                        digestTruncated = true;
                        break;
                    }
                    files.append(p);
                    ++digestEmitted;
                }
                l[QStringLiteral("source_files")] = files;
            }
            lanes.append(l);
        }
        // ANTS-3503 — no-module-map fallback. When the lane digest is requested
        // but nothing was emitted (no file carries a lane → the project has no
        // parseable `## Module map` to partition on, e.g. finbreak), the summary
        // still can't answer "where is the code". Emit a flat, capped, sorted
        // non-test file-path digest so lane-less repos get a navigable first-call
        // map too. Deterministic (sorted) → keeps session_orient's 304 ETag
        // stable; shares the kMaxLaneDigestFiles cap + the lane_digest_truncated
        // flag. Only present when the fallback fires, so the lane-digest shape is
        // byte-identical for a project that does have lanes.
        if (params.laneFiles && digestEmitted == 0) {
            QStringList flat;
            for (const FileEntry &fe : idx.files)
                if (fe.role != QLatin1String("test"))
                    flat << fe.path;
            flat.sort();
            QJsonArray files;
            for (const QString &p : flat) {
                if (files.size() >= opts.maxLaneDigestFiles) {
                    digestTruncated = true;
                    break;
                }
                files.append(p);
            }
            env[QStringLiteral("source_files")] = files;
        }
        QJsonObject langs, roles;
        for (auto it = langCounts.cbegin(); it != langCounts.cend(); ++it)
            langs[it.key()] = it.value();
        for (auto it = roleCounts.cbegin(); it != roleCounts.cend(); ++it)
            roles[it.key()] = it.value();
        env[QStringLiteral("lane_count")] = laneCounts.size();
        env[QStringLiteral("lanes")]      = lanes;
        env[QStringLiteral("languages")]  = langs;
        env[QStringLiteral("roles")]      = roles;
        // ANTS-2148 — soft empty-signal. An empty map (file_count:0) is
        // otherwise indistinguishable from a tiny project, so a consuming
        // session can't tell "no source admitted" from "nothing here" and
        // may trust an empty index instead of falling back to grep. The
        // flag is stable while the tree is unchanged (it does not perturb
        // session_orient's 304 ETag, unlike generated_at_ms).
        env[QStringLiteral("empty")] = idx.files.isEmpty();
        // ANTS-3468 — surface the digest cap only when the digest was emitted
        // (keeps the counts-only summary shape byte-identical when opt-out).
        if (params.laneFiles)
            env[QStringLiteral("lane_digest_truncated")] = digestTruncated;
    } else if (!params.symbol.isEmpty()) {
        QJsonArray matches;
        for (const FileEntry &fe : idx.files)
            for (const Symbol &s : fe.symbols)
                if (s.name == params.symbol) {
                    QJsonObject m;
                    m[QStringLiteral("path")] = fe.path;
                    m[QStringLiteral("line")] = s.line;
                    m[QStringLiteral("kind")] = s.kind;
                    matches.append(m);
                }
        env[QStringLiteral("found")]   = !matches.isEmpty();
        env[QStringLiteral("matches")] = matches;
    } else if (!params.lane.isEmpty()) {
        if (!idx.laneToFiles.contains(params.lane)) {
            QJsonArray names;
            for (auto it = idx.laneToFiles.cbegin(); it != idx.laneToFiles.cend(); ++it)
                names.append(it.key());
            env[QStringLiteral("found")] = false;
            env[QStringLiteral("lane")]  = params.lane;
            env[QStringLiteral("lanes")] = names;
        } else {
            env[QStringLiteral("found")] = true;
            env[QStringLiteral("lane")]  = params.lane;
            QJsonArray files;
            int emitted = 0;
            for (const QString &rel : idx.laneToFiles.value(params.lane)) {
                const FileEntry *fe = findFile(idx, rel);
                if (!fe) continue;
                QJsonObject fo;
                fo[QStringLiteral("path")]     = fe->path;
                fo[QStringLiteral("role")]     = fe->role;
                fo[QStringLiteral("language")] = fe->language;
                fo[QStringLiteral("lines")]    = fe->lines;
                QJsonArray syms;
                for (const Symbol &s : fe->symbols) {
                    if (emitted >= opts.maxQuerySymbols) { symbolsTruncated = true; break; }
                    syms.append(symbolJson(s));
                    ++emitted;
                }
                fo[QStringLiteral("symbols")] = syms;
                files.append(fo);
            }
            env[QStringLiteral("files")] = files;
        }
    } else {  // file_path
        const FileEntry *fe = findFile(idx, params.filePath);
        env[QStringLiteral("found")] = fe != nullptr;
        if (fe) env[QStringLiteral("entry")] = fileEntryJson(*fe, /*withLane=*/true);
    }

    env[QStringLiteral("symbols_truncated")] = symbolsTruncated;
    return env;
}

QJsonObject toJson(const Index &idx) {
    QJsonObject o;
    o[QStringLiteral("version")]          = idx.version;
    o[QStringLiteral("root_canonical")]   = idx.rootCanonical;
    o[QStringLiteral("generated_at_ms")]  = idx.generatedAtMs;
    o[QStringLiteral("map_source_mtime_ms")] = idx.mapSourceMtimeMs;
    o[QStringLiteral("files_truncated")]  = idx.filesTruncated;
    QJsonArray files;
    for (const FileEntry &fe : idx.files) {
        QJsonObject f;
        f[QStringLiteral("path")]     = fe.path;
        f[QStringLiteral("language")] = fe.language;
        f[QStringLiteral("role")]     = fe.role;
        f[QStringLiteral("lane")]     = fe.lane;
        f[QStringLiteral("lines")]    = fe.lines;
        f[QStringLiteral("mtime_ms")] = fe.mtimeMs;
        f[QStringLiteral("symbols")]  = symbolsJson(fe.symbols);
        files.append(f);
    }
    o[QStringLiteral("files")] = files;
    return o;
}

Index fromJson(const QJsonObject &obj) {
    Index idx;
    idx.version          = obj.value(QStringLiteral("version")).toInt();
    idx.rootCanonical    = obj.value(QStringLiteral("root_canonical")).toString();
    idx.generatedAtMs    = static_cast<qint64>(
        obj.value(QStringLiteral("generated_at_ms")).toDouble());
    idx.mapSourceMtimeMs = static_cast<qint64>(
        obj.value(QStringLiteral("map_source_mtime_ms")).toDouble());
    idx.filesTruncated   = obj.value(QStringLiteral("files_truncated")).toBool();
    for (const QJsonValue &v : obj.value(QStringLiteral("files")).toArray()) {
        const QJsonObject f = v.toObject();
        FileEntry fe;
        fe.path     = f.value(QStringLiteral("path")).toString();
        fe.language = f.value(QStringLiteral("language")).toString();
        fe.role     = f.value(QStringLiteral("role")).toString();
        fe.lane     = f.value(QStringLiteral("lane")).toString();
        fe.lines    = f.value(QStringLiteral("lines")).toInt();
        fe.mtimeMs  = static_cast<qint64>(f.value(QStringLiteral("mtime_ms")).toDouble());
        for (const QJsonValue &sv : f.value(QStringLiteral("symbols")).toArray()) {
            const QJsonObject so = sv.toObject();
            Symbol s;
            s.name = so.value(QStringLiteral("name")).toString();
            s.line = so.value(QStringLiteral("line")).toInt();
            s.kind = so.value(QStringLiteral("kind")).toString();
            fe.symbols << s;
        }
        idx.files << fe;
    }
    rebuildLaneToFiles(idx);
    return idx;
}

QString cachePathFor(const QString &rootCanonical) {
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::GenericCacheLocation);
    return base + QStringLiteral("/ants-terminal/codebase-index/")
         + SessionMemoryEngine::cwdHash(rootCanonical) + QStringLiteral(".json");
}

QJsonObject serve(const QString &rootCanonical, qint64 nowMs,
                  const QueryParams &params, const Options &opts,
                  const QString &cachePathOverride) {
    const QString cachePath =
        cachePathOverride.isEmpty() ? cachePathFor(rootCanonical) : cachePathOverride;
    const qint64 curMapMtime = mapSourceMtimeMs(rootCanonical);

    Index idx;
    int refreshed = 0;
    bool needWrite = false;

    // Load + validate the cache (cold-build triggers: absent / unparseable /
    // version mismatch / root mismatch — INV-13).
    bool haveValid = false;
    Index prev;
    {
        QFile f(cachePath);
        if (f.open(QIODevice::ReadOnly)) {
            QJsonParseError pe{};
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
            if (pe.error == QJsonParseError::NoError && doc.isObject()) {
                Index loaded = fromJson(doc.object());
                if (loaded.version == kIndexVersion
                    && loaded.rootCanonical == rootCanonical) {
                    prev = loaded;
                    haveValid = true;
                }
            }
        }
    }

    if (haveValid) {
        // ANTS-2198 — compute candidates() (loads .ants/project.json) + the
        // stale set ONCE and thread both into refreshWith, instead of letting
        // staleFiles + refresh each re-walk the tree.
        const QStringList cands = candidates(rootCanonical);
        const StaleSet ss = staleFilesWith(prev, rootCanonical, curMapMtime, cands);
        if (ss.any()) {
            idx = refreshWith(prev, rootCanonical, nowMs, curMapMtime, opts,
                              &refreshed, ss, cands);
            needWrite = true;
        } else {
            idx = prev;  // fully warm — no write, generated_at_ms preserved
        }
    } else {
        idx = build(rootCanonical, nowMs, opts);
        refreshed = idx.files.size();
        needWrite = true;
    }

    if (needWrite) {
        QDir().mkpath(QFileInfo(cachePath).absolutePath());
        QSaveFile sf(cachePath);
        if (sf.open(QIODevice::WriteOnly)) {
            sf.write(QJsonDocument(toJson(idx)).toJson(QJsonDocument::Compact));
            sf.commit();
        }
    }

    QJsonObject env = query(idx, params, refreshed, cachePath, opts);

    // ANTS-4419 — say WHY an empty index is empty. ANTS-2148 added the `empty`
    // boolean for exactly this reason ("a consuming session can't tell 'no
    // source admitted' from 'nothing here'") and stopped at a boolean, so a
    // caller still had nothing to act on. Reported by a Charls_Site session
    // that concluded "there is no code here" on a tree of ~1300 .html + 4 .py
    // files and fell back to grep — against the SessionStart hook's own advice
    // to query the index first.
    //
    // The condition is NOT the missing .git that report first proposed: the
    // same session's A/B test disproved it (the tree became a git repo, 1426
    // files tracked, and the index stayed empty). candidates() walks src/ +
    // tests/ unless .ants/project.json declares source_roots, so the real
    // condition is "the roots that were walked hold no indexable source".
    //
    // Reuses ANTS-2161's detector rather than adding a second layout analysis
    // — one owner for the question. Cost is bounded and paid only on the
    // already-empty path (the posture ANTS-3560's id-bearing scan takes), and
    // detect() short-circuits with no walk at all when the settings file
    // exists. Every field is a deterministic function of the tree, so
    // session_orient's 304 ETag stays stable (unlike generated_at_ms).
    //
    // Gated on query()'s own `empty` rather than on idx.files.isEmpty(), so
    // this fires exactly where ANTS-2148's flag does — the summary path — and
    // a symbol / lane / file_path response keeps its byte-identical shape.
    if (env.value(QStringLiteral("empty")).toBool()) {
        const ProjectSettings::Suggestion sug =
            ProjectSettings::detect(rootCanonical);
        // ORDER IS LOAD-BEARING. detect() does not walk when the settings file
        // is present, so totalSourceCount is 0 there BY CONSTRUCTION — testing
        // the count first would report a registered project as genuinely
        // empty, which is the same class of mistake as the missing-.git
        // diagnosis this item exists to correct.
        if (sug.present) {
            env[QStringLiteral("empty_reason")] =
                QStringLiteral("declared_roots_hold_no_source");
            env[QStringLiteral("empty_hint")] = QStringLiteral(
                "an .ants/project.json is present, but the source_roots it "
                "declares hold no indexable file. Check them against the "
                "tree with project_settings op:detect.");
        } else if (sug.totalSourceCount > 0) {
            env[QStringLiteral("empty_reason")] =
                QStringLiteral("project_not_registered");
            env[QStringLiteral("empty_hint")] = QStringLiteral(
                "this tree holds indexable source, but not under src/ or "
                "tests/, and no .ants/project.json says where it lives. Run "
                "project_settings op:detect then op:init to register it. The "
                "index is INAPPLICABLE here, not authoritative — do not read "
                "this as \"no code here\".");
        } else {
            env[QStringLiteral("empty_reason")] =
                QStringLiteral("no_indexable_source");
            env[QStringLiteral("empty_hint")] = QStringLiteral(
                "no file with an indexable suffix was found anywhere in this "
                "tree, so the empty index is accurate.");
        }
        // detect()'s own sentence, which carries the measured counts. Always
        // non-empty after a walk or a short-circuit (ANTS-3369), so a caller
        // gets the numbers without a second call.
        if (!sug.reason.isEmpty())
            env[QStringLiteral("empty_detail")] = sug.reason;
    }
    return env;
}

}  // namespace CodebaseIndex
