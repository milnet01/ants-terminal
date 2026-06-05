// ANTS-1637 — codebase_index MCP tool: a pre-computed project structural map.
// Pure helper (Qt6::Core-only, FS-reading, no widgets / no subprocess —
// mirrors findsources.h). Builds {symbols-per-file, lane→files} by walking
// <root>/src + <root>/tests and reusing FileOutline + SubsystemMap, caches the
// result to ~/.cache/ants-terminal/codebase-index/<hash>.json, and serves
// symbol / lane / file_path / summary queries so a session stops re-deriving
// the project shape with grep / file_outline / CLAUDE.md reads.
//
// The thin cmdCodebaseIndex wrapper (remotecontrol.cpp) resolves the project
// root + the caller_cwd contract + PathValidation, then drives serve() here.
// See docs/specs/ANTS-1637.md.

#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace CodebaseIndex {

constexpr int    kIndexVersion    = 1;
constexpr int    kMaxIndexFiles   = 5000;             // INV-11 file-count ceiling
constexpr qint64 kMaxCacheBytes   = 16 * 1024 * 1024; // INV-15 hard cache/heap ceiling
constexpr int    kMaxQuerySymbols = 2000;             // INV-14 lane= response cap

// A subset of FileOutline's symbol entry (signature dropped — INV/§2.1).
struct Symbol {
    QString name;   // qualified as FileOutline emits, e.g. "TestAuditEngine::foldIn"
    int     line = 0;
    QString kind;   // FileOutline's kind verbatim: "func" | "class" | "qt"
};

struct FileEntry {
    QString         path;       // project-relative ("src/testauditengine.cpp")
    QString         language;   // "cpp" | "py" in v1
    QString         role;       // "impl" | "header" | "test"
    QString         lane;       // "" when no lane resolves
    int             lines = 0;
    qint64          mtimeMs = 0;
    QVector<Symbol> symbols;
};

struct Index {
    int                        version = kIndexVersion;
    QString                    rootCanonical;
    qint64                     generatedAtMs = 0;
    qint64                     mapSourceMtimeMs = 0;  // docs/subsystems.md|CLAUDE.md mtime
    QVector<FileEntry>         files;            // sorted: src/ (sorted) then tests/ (sorted)
    QMap<QString, QStringList> laneToFiles;      // lane → [relative paths], each list sorted
    bool                       filesTruncated = false;
};

struct Options {
    int    maxIndexFiles   = kMaxIndexFiles;
    qint64 maxCacheBytes   = kMaxCacheBytes;
    int    maxQuerySymbols = kMaxQuerySymbols;
};

// At most one member non-empty: that one picks the filter; none → summary;
// query() rejects ≥2 non-empty with bad_args.
struct QueryParams {
    QString symbol;
    QString lane;
    QString filePath;   // project-relative (validated by the handler)
};

struct StaleSet {
    QStringList changed, added, removed;
    bool        mapSourceChanged = false;
    bool        any() const {
        return mapSourceChanged || !changed.isEmpty() || !added.isEmpty()
            || !removed.isEmpty();
    }
};

// mtime (ms) of the canonical lane-map source (docs/subsystems.md when
// present, else CLAUDE.md) for `rootCanonical`; 0 if neither exists.
qint64 mapSourceMtimeMs(const QString &rootCanonical);

// Cold build over <root>/src + <root>/tests (indexed-suffix set), one
// FileOutline pass per file, lane assignment + laneToFiles inversion. Stops
// at maxIndexFiles or maxCacheBytes (filesTruncated). generatedAtMs injected.
Index build(const QString &rootCanonical, qint64 generatedAtMs,
            const Options &opts = {});

// changed/added/removed deltas vs `prev` plus a map-source-mtime change.
StaleSet staleFiles(const Index &prev, const QString &rootCanonical,
                    qint64 currentMapSourceMtimeMs);

// Re-outline only changed+added files, drop removed, reuse prev symbols for
// untouched files, re-derive laneToFiles. *refreshedOut = changed+added count.
Index refresh(const Index &prev, const QString &rootCanonical,
              qint64 generatedAtMs, qint64 currentMapSourceMtimeMs,
              const Options &opts, int *refreshedOut);

// Build the response envelope (without the dispatcher-injected etag) for one
// query against an in-memory index. refreshedFiles + cachePath ride the meta
// block. ≥2 selectors → bare {ok:false, code:bad_args}.
QJsonObject query(const Index &idx, const QueryParams &params,
                  int refreshedFiles, const QString &cachePath,
                  const Options &opts = {});

QJsonObject toJson(const Index &idx);
Index       fromJson(const QJsonObject &obj);

// ~/.cache/ants-terminal/codebase-index/<cwdHash(root)>.json.
QString cachePathFor(const QString &rootCanonical);

// Orchestrate load → refresh → write-back → query. Cold-builds when the cache
// is absent / unparseable / version- or root-mismatched. nowMs injected for
// testability; cachePathOverride empty → cachePathFor(root).
QJsonObject serve(const QString &rootCanonical, qint64 nowMs,
                  const QueryParams &params, const Options &opts = {},
                  const QString &cachePathOverride = {});

}  // namespace CodebaseIndex
