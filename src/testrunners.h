// ANTS-5503 — the test runners `test_results` and `focused_test` understand.
//
// Pure (Qt6::Core only): finds a project's runner from its marker files,
// recognises and parses each runner's text output into the same
// TestResCache::ParsedTests ctest fills, and builds the command
// focused_test runs. Spawns nothing; the only I/O is reading files under
// the project root.
//
// Layout / invariants documented in docs/specs/ANTS-5503-multi-runner-tests.md.

#pragma once

#include "focusedtest.h"
#include "testrescache.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace TestRunners {

enum class Runner { Ctest, Pytest, Jest, Vitest, Cargo, Go };

QString name(Runner r);                           // "ctest", "pytest", ...
std::optional<Runner> fromName(const QString &s); // exact, lower-case
QString allNames();                               // "ctest, pytest, ..." for messages

struct Detection {
    std::optional<Runner> runner;  // empty = none found
    QString evidence;              // root-relative marker, e.g. "go.mod"
};
// § 2.2. Reads marker files under rootCanonical; spawns nothing.
Detection detect(const QString &rootCanonical);

// § 2.3. The first runner whose output shape matches, else empty.
std::optional<Runner> sniff(const QString &output);

// § 2.3. Pure. Fills the same caps ParsedTests carries for ctest.
TestResCache::ParsedTests parse(Runner r, const QString &output);

// § 2.4 — the command focused_test runs for a runner other than ctest,
// which keeps FocusedTest::resolve.
struct Command {
    QString     program;      // "python3", "npx", "cargo", "go"
    QStringList args;         // the narrowed run, or the full run
    QStringList fullArgs;     // the full run, for the 0-match net
    QStringList selector;     // the args that narrowed the run; empty = full
    QString     selection;    // "heuristic" / "related" / "full"
    QString     reason;
    QStringList mappedFiles;
    QStringList unmappedFiles;
    QStringList ignoredFiles;
};
// Reads Cargo.toml files (cargo) and walks the tree (pytest); otherwise pure.
Command buildCommand(Runner r, const QString &rootCanonical,
                     const QStringList &changedFiles,
                     const FocusedTest::CoverageMap &map);

}  // namespace TestRunners
