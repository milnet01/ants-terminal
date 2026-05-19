// ANTS-1299 — `build_status` MCP cache.
//
// Provides the `<project>/.audit_cache/build.json` cache that
// powers the `build_status` MCP tool: parses GCC/clang/cppcheck-2.x
// compiler output into an errors[] + warnings_count summary, writes
// atomically via QSaveFile, reads on demand. No GUI deps.
//
// Layout / invariants documented in `docs/specs/ANTS-1299.md`.

#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace BuildCache {

struct ParsedError {
    QString file;
    int     line = 0;
    QString severity;  // "error" or "warning" (note continuations folded into parent error)
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

// `<root>/.audit_cache/build.json`. Empty when canonProject empty.
QString cachePath(const QString &canonProject);

// Parse a GCC/clang/cppcheck-2.x build log into ParsedBuild.
// Pure function. Caps errors[] at 50 entries (errorsTruncated:true
// when more were seen); per-message text capped at 240 chars.
ParsedBuild parseBuildOutput(const QString &output);

// Write `<root>/.audit_cache/build.json` atomically via QSaveFile,
// 0600 perms. Sets recordedAtMs on the in-memory ParsedBuild as a
// side effect (caller may want to surface it). Returns false on
// mkdir / write failure.
bool recordBuild(const QString &canonProject, ParsedBuild &build);

// Read + parse the cache file. Returns empty optional on missing
// file, malformed JSON, or schema_version != 1.
std::optional<ParsedBuild> loadBuild(const QString &canonProject);

// Walk the project's compile-input set looking for any file with
// mtime > recordedAtMs. Returns {stale_known, stale}. When
// stale_known is false, the walk hit its 5 000-file cap before any
// newer file was found — callers report `stale_walk_capped:true`
// instead of `stale:false`. The probed set is per § 3.3.2 step 3
// of the spec.
struct StaleCheck {
    bool staleKnown = true;  // false ⇒ walk capped, stale unknown
    bool stale      = false;
};
StaleCheck checkStale(const QString &canonProject, qint64 recordedAtMs);

// Render to/from JSON object (1:1 with the on-disk shape). Caller
// uses this when building the MCP envelope.
QJsonObject toJson(const ParsedBuild &build);
ParsedBuild fromJson(const QJsonObject &obj, bool *okOut = nullptr);

}  // namespace BuildCache
