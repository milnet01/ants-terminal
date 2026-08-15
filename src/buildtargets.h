// ANTS-3745 — "which build target owns this file?", answered from
// CMakeLists.txt instead of by hand.
//
// Qt6::Core only, in ants_core_lib beside speclint.h / fileoutline.h, so it is
// unit-testable without RemoteControl / MainWindow. It opens nothing: the text
// is handed in, the family convention ANTS-3664 § 2.3 states.
//
// Why it exists. After editing a `tests/features/<x>/test_<x>.cpp` you must
// know its bundle to build anything narrower than everything, and nothing in
// the toolkit said. The two fallbacks in use were an `awk` walking backwards
// from the file's line to the nearest `ants_add_*_bundle`, and — worse —
// running every `build/test_*` binary with `--gtest_list_tests` to find which
// one carried the suite. CLAUDE.md's own `--target <bundle>` advice assumes
// the caller already knows the bundle, so this is the missing half of
// documented guidance rather than a new idea.
//
// The mapping is NOT guessable from the path, which is why recall does not
// substitute: `tests/features/cold_eyes_engine/` builds into `test_audit`, and
// `tests/features/spec_conformance/` into `test_claude`.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace BuildTargets {

struct Target {
    QString     name;
    // "library" | "executable" | "bundle" — the declaring command's family,
    // not a guess from the name. A bundle is one of this project's
    // `ants_add_*_bundle()` wrappers, which expand to add_executable.
    QString     kind;
    QString     command;    // the literal command, e.g. "ants_add_gui_bundle"
    int         line = 0;   // 1-based line of the declaring command
    QStringList sources;    // project-relative, exactly as written
};

// Every add_library / add_executable / ants_add_*_bundle block in `cmakeText`.
//
// Sources are collected by SHAPE (a token containing `/` and ending in a C or
// C++ source or header suffix) rather than by tracking the keyword sections,
// because the two bundle wrappers take only `LIBS;SOURCES` and a library name
// is never path-shaped. Deliberately looser than a CMake parser: it must not
// invent a target, and a token it fails to recognise costs one lookup that
// falls back to `found:false` — which is the honest answer this file exists to
// replace a wrong guess with.
//
// Not resolved: a source named through a variable, a generator expression, or
// `target_sources()`. None appears in this project's lists; a file reached only
// that way is reported unowned rather than attributed to the wrong target.
QList<Target> parse(const QString &cmakeText);

// The targets whose SOURCES name `relPath`, in declaration order. A list
// rather than one target: a test source compiled into two bundles is legal,
// and returning the first would send a caller to build half of what changed.
QList<Target> ownersOf(const QList<Target> &targets, const QString &relPath);

// The googletest suite names a source declares — the `Suite` of every
// `TEST(Suite, Name)` / `TEST_F` / `TEST_P`, deduplicated, in first-seen order.
// This is what makes `ctest -R` usable straight from the answer, and it is the
// half the `--gtest_list_tests` fallback was paying a process launch for.
QStringList gtestSuites(const QString &sourceText);

}  // namespace BuildTargets
