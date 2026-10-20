#pragma once

// The source files a code-reading verb looks at (ANTS-5785, 5786, 5787):
// `paths[]` under the project root resolved into files, with tests and
// generated files left out unless asked for. Shared so the readability
// verbs agree on what "the code" is. Its own translation unit, outside
// ANTS_RC_SOURCES_REL, so a test links it without RemoteControl.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace CodeWalk {

inline constexpr int kMaxFiles = 20000;

struct Options {
    bool includeTests = false;
    bool includeGenerated = false;
};

struct Result {
    QJsonObject refusal;      // non-empty: {ok:false, code, error}; nothing else is set
    QStringList files;        // canonical absolute paths, sorted
    int skippedTests = 0;
    int skippedGenerated = 0;
    bool truncated = false;   // stopped at kMaxFiles
};

// `paths` holds files or directories, project-relative or absolute, each
// checked by PathValidation (bad_path); a non-string entry refuses bad_args.
// Empty: the project's source_roots (.ants/project.json), else `src`, else the
// root. A directory is walked for files CodebaseIndex indexes, pruning
// isNoiseDir names and what git ignores. A file named directly is kept
// whatever its kind. `verb` names the caller in a refusal.
Result collect(const QString &rootCanonical, const QJsonArray &paths,
               const Options &opts, const QString &verb);

// ANTS-4120 — the generated-file predicate, by file name: Qt's moc_/ui_/qrc_
// outputs, shader byte arrays, protobuf, flex/bison and `*_generated.<ext>`.
// Moved here from indiereviewengine.cpp; auditdialog and findsources still
// keep their own copies.
bool isGeneratedSource(const QString &fileName);

// True for a test file: a path component named tests, test, __tests__ or
// spec, a path under the project's declared test_roots, or a name shaped
// test_*, *_test.*, *_tests.*, *.test.* or *.spec.*. `relPath` is
// root-relative with forward slashes.
bool isTestPath(const QString &relPath, const QStringList &testRoots);

// The envelope fields every readability verb reports beside its result, so
// an empty answer says what it looked at (mcp-tools.md step 5a).
QJsonObject scannedSummary(const Result &r);

} // namespace CodeWalk
