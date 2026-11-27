// ANTS-1299 — `build_status` MCP cache.
//
// Provides the `<project>/.audit_cache/build.json` cache that
// powers the `build_status` MCP tool: parses GCC/clang/cppcheck-2.x
// compiler output into an errors[] + warnings_count summary, writes
// atomically via QSaveFile, reads on demand. No GUI deps.
//
// Layout / invariants documented in `docs/specs/ANTS-1299-build-status-mcp-cached-build.md`.

#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace BuildCache {

struct ParsedError {
    QString file;
    int     line = 0;
    QString severity;  // always "error": warnings are only counted, notes fold into their error
    QString message;
};

struct ParsedBuild {
    int           exitCode = 0;
    qint64        startedAtMs    = 0;   // 0 = not supplied
    qint64        finishedAtMs   = 0;   // 0 = not supplied
    qint64        durationMs     = -1;  // -1 = not supplied; otherwise echoed verbatim
    QList<ParsedError> errors;
    int           errorsCount   = 0;    // total parsed, may exceed errors.size() when truncated
    bool          errorsTruncated = false;
    int           warningsCount = 0;
    qint64        outputByteSize = 0;
    qint64        recordedAtMs   = 0;   // 0 on freshly-parsed pre-record; filled at write time
};

// The writer's caps; fromJson keeps them too (ANTS-6528).
inline constexpr int kMaxErrors      = 50;     // errors[] entries
inline constexpr int kMessageCharCap = 240;    // chars per message
// checkStale stops after this many files.
inline constexpr int kStaleWalkCap   = 5'000;

// `<root>/.audit_cache/build.json`. Empty when canonProject empty.
QString cachePath(const QString &canonProject);

// Parse a GCC/clang/cppcheck-2.x build log into ParsedBuild.
// Pure function. Keeps kMaxErrors errors (errorsTruncated:true when
// more were seen), each message cut at kMessageCharCap chars.
ParsedBuild parseBuildOutput(const QString &output);

// Write `<root>/.audit_cache/build.json` atomically via QSaveFile,
// 0600 perms. Sets recordedAtMs on the in-memory ParsedBuild as a
// side effect (caller may want to surface it). Returns false for an
// empty project or a mkdir, write or commit failure, and when the
// committed file's 0600 cannot be set, after removing that file.
bool recordBuild(const QString &canonProject, ParsedBuild &build);

// Read + parse the cache file. Returns empty optional on missing
// file, malformed JSON, schema_version != 1, or (ANTS-6528) a file
// over kMaxCacheFileBytes, which is not read whole.
inline constexpr qint64 kMaxCacheFileBytes = qint64{256} * 1024;
std::optional<ParsedBuild> loadBuild(const QString &canonProject);

// The time checkStale compares against: when the build started, if the
// caller supplied it, else when it was recorded. A file edited during the
// build may be missing from its result.
qint64 staleSinceMs(const ParsedBuild &build);

// Walk the project's compile-input set looking for any file with
// mtime > sinceMs. Returns {stale_known, stale}. When
// stale_known is false, the walk hit kStaleWalkCap before any
// newer file was found, or sinceMs is not a time — callers report
// `stale_walk_capped:true` instead of `stale:false`. The probed set is
// per § 3.3 (`op=read`) step 3 of the spec.
struct StaleCheck {
    bool staleKnown = true;  // false ⇒ stale unknown
    bool stale      = false;
};
StaleCheck checkStale(const QString &canonProject, qint64 sinceMs);

// Render to/from JSON object (1:1 with the on-disk shape). Caller
// uses this when building the MCP envelope. fromJson keeps the writer's
// caps (ANTS-6528): kMaxErrors and kMessageCharCap.
QJsonObject toJson(const ParsedBuild &build);
ParsedBuild fromJson(const QJsonObject &obj, bool *okOut = nullptr);

}  // namespace BuildCache
