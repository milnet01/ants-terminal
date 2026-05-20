// ANTS-1302 — `focused_test` resolution lib.
//
// Pure (Qt6::Core only) logic that maps a set of changed files to the
// `ctest -R` patterns that should be run, via a project-supplied
// `tests/coverage-map.json`, a heuristic, or a full-suite fallback.
// No process spawning, no writes — the only I/O is reading the
// coverage-map file. The MCP layer (`RemoteControl::cmdFocusedTest`)
// runs ctest and parses it with `TestResCache::parseCtestOutput`.
//
// Layout / invariants documented in `docs/specs/ANTS-1302.md`.

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace FocusedTest {

enum class Selection { Map, Heuristic, Full };

struct CoverageMap {
    bool        valid = false;   // parsed OK and schema_version == 1
    QString     error;           // "absent" / "bad_json" / "bad_schema" / ""
    int         schemaVersion = 0;
    QHash<QString, QStringList> entries;  // repo-rel src path -> patterns
    QStringList defaultPatterns;          // optional top-level "default"
    QStringList ignoreGlobs;              // optional top-level "ignore"
};

// Read <root>/tests/coverage-map.json. Missing file -> {valid:false,
// error:"absent"}. Malformed JSON -> error:"bad_json". schema_version
// != 1 -> error:"bad_schema". Never throws.
CoverageMap loadCoverageMap(const QString &rootCanonical);

struct Resolution {
    Selection   selection = Selection::Full;
    QStringList patterns;        // ctest -R alternatives (empty == run all)
    QStringList mappedFiles;     // changed files that hit a map/heuristic entry
    QStringList unmappedFiles;   // changed files with no entry, not ignored
    QStringList ignoredFiles;    // changed files matched by an ignore glob
    QString     reason;          // human-readable selection rationale
};

// Resolve changed files (repo-relative, leading "./" already stripped)
// to ctest patterns. See docs/specs/ANTS-1302.md § 3.3.
Resolution resolve(const QStringList &changedFiles, const CoverageMap &map);

// foo.cpp / foo.h -> a best-effort ctest-name fragment (the basename
// stem). Best-effort only; the 0-match safety net in the MCP layer
// covers a heuristic that matches nothing. Empty for a file with no
// usable stem.
QString heuristicPattern(const QString &changedFile);

// OR-join patterns into one ctest -R ERE: "(p1|p2|p3)". Single
// pattern -> "(p1)". Empty list -> "".
QString buildCtestRegex(const QStringList &patterns);

}  // namespace FocusedTest
