// ANTS-1636 — find_sources: topic-to-files discovery for subsystems
// that may not be in the parsed Module map. Pure function unit; no
// QProcess, no Qt::Widgets. Caller passes a project root + a free-text
// topic; returns a ranked file list with role/evidence per entry.
//
// Design constraints:
//   - O(files) walk over <root>/src/ + <root>/tests/. No recursion
//     outside those two prefixes — find_sources is for source code,
//     not docs/, packaging/, etc.
//   - Per-file content scan capped to a small byte budget so the tool
//     stays cheap on large source files (the goal is "where might
//     this be", not "exhaustive grep").
//   - No external subprocess. ripgrep is fast but layering its
//     timeout / SIGKILL / stderr handling adds 200+ lines of QProcess
//     scaffolding for a tool that searches at most a few MB of text.

#ifndef ANTS_FINDSOURCES_H
#define ANTS_FINDSOURCES_H

#include <QString>
#include <QStringList>
#include <QVector>

namespace FindSources {

struct FileHit {
    QString     path;        // project-relative ("src/foo.cpp")
    int         score = 0;   // higher = more relevant
    QString     role;        // "impl" | "header" | "test"
    QStringList evidence;    // human-readable hit explanations (≤ 3)
};

struct Result {
    QVector<FileHit> files;
    QStringList      unmatchedTerms;  // input tokens with zero file hits
    int              filesScanned = 0;
    bool             truncated = false;
};

struct Options {
    int maxResults     = 20;          // INV-3 default cap
    int contentByteCap = 256 * 1024;  // INV-4 per-file scan budget
    int maxResultsHard = 100;         // INV-3 hard cap (server clamp)
};

// Tokenise a free-text topic into search tokens. Splits on whitespace
// / punctuation; drops tokens shorter than 3 chars; drops `ANTS-NNNN`
// roadmap IDs (they shouldn't be searched literally). Exposed for the
// tests; the resolveTopic() pipeline calls it internally.
QStringList tokenise(const QString &topic);

// Expand a single token into all its likely casing/separator variants
// (lowercase, snake_case, camelCase, dropped-separator). De-duplicated;
// case-insensitive matching is the caller's job (the variants here are
// for filename matching).
QStringList variantsForToken(const QString &token);

// Resolve a free-text topic to ranked source files under projectRoot.
// projectRoot must already be a canonical existing directory. Returns
// an empty Result on a bad root.
Result findSources(const QString &topic,
                   const QString &projectRoot,
                   const Options &opts = {});

}  // namespace FindSources

#endif
