// ANTS-1300 — `test_results` MCP cache.
//
// Provides the `<project>/.audit_cache/tests.json` cache that powers
// the `test_results` MCP tool: parses ctest --output-on-failure into
// pass/fail/skip counts + per-failure excerpts, writes atomically,
// reads on demand. No GUI deps.
//
// Layout / invariants documented in `docs/specs/ANTS-1300.md`.

#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace TestResCache {

struct ParsedFailure {
    QString name;
    QString excerpt;      // last 20 lines, per-line capped at 1000, total capped at 4000
    QString fullExcerpt;  // whole block, capped at 8000; on-disk only, NOT echoed
};

struct ParsedTests {
    int    exitCode = 0;
    int    passed   = 0;
    int    failed   = 0;
    int    skipped  = 0;
    int    total    = 0;
    QList<ParsedFailure> failingTests;
    bool   failingTestsTruncated = false;
    qint64 startedAtMs  = 0;   // 0 = not supplied
    qint64 finishedAtMs = 0;   // 0 = not supplied
    qint64 durationMs   = -1;  // -1 = not supplied / not extracted
    qint64 outputByteSize = 0;
    qint64 recordedAtMs   = 0;
    bool   recognised    = false;  // false = neither summary nor per-test line matched
};

// `<root>/.audit_cache/tests.json`. Empty when canonProject empty.
QString cachePath(const QString &canonProject);

// Parse a ctest --output-on-failure log into ParsedTests. Pure
// function. Caps failingTests[] at 50; per-failure full body at
// 8000 chars; per-line excerpt at 1000 chars; joined excerpt at
// 4000 chars. Sets `recognised:false` if neither summary footer
// nor any per-test status line matched (caller refuses with
// bad_args).
ParsedTests parseCtestOutput(const QString &output);

// Write `<root>/.audit_cache/tests.json` atomically via QSaveFile,
// 0600 perms. Sets recordedAtMs on the in-memory ParsedTests.
// Returns false on mkdir / write failure.
bool recordTests(const QString &canonProject, ParsedTests &tests);

// Read + parse the cache file. Returns empty optional on missing
// file, malformed JSON, or schema_version != 1.
std::optional<ParsedTests> loadTests(const QString &canonProject);

// On-disk shape (carries full_excerpt). Caller uses toJsonOnDisk for
// recordTests and toJsonWire(...) to build the MCP envelope on
// `op=read` (strips full_excerpt from each failing test).
QJsonObject toJsonOnDisk(const ParsedTests &tests);
QJsonObject toJsonWire(const ParsedTests &tests);
ParsedTests fromJson(const QJsonObject &obj, bool *okOut = nullptr);

}  // namespace TestResCache
