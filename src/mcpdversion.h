// ANTS-5340 — which ants-mcpd build Claude Code launches, for Help → About.
//
// Since ANTS-4932 ants-mcpd is rebuilt and reconnected on its own, so its
// build can differ from the terminal's. The About dialog reads it when it
// opens, so a rebuilt ants-mcpd shows with no terminal relaunch.
// Contract: tests/features/mcpd_about_version/spec.md.

#pragma once

#include <QList>
#include <QString>

namespace mcpd {

// "ants-mcpd <version> · <date> <time> (<type>) · commit <sha>". What
// `ants-mcpd --version` prints.
QString versionLine();

// The ants-mcpd Claude Code launches. In order: the user-level
// `mcpServers.ants.command` in `claudeJsonPath`, then `appDir/ants-mcpd`, then
// a PATH search. Each must be executable. Empty when none is.
QString locateBinary(const QString &claudeJsonPath, const QString &appDir);

// Runs `<binary> --version` and returns its first stdout line, or an empty
// string when it does not start, times out, or exits non-zero.
QString queryVersion(const QString &binary, int timeoutMs);

// ANTS-5341 — one running ants-mcpd. Each Claude Code session runs its own
// until it reconnects, so after a rebuild some still run the older one.
struct RunningCopy {
    qint64 pid = 0;
    // Its own `--version` line, asked of /proc/<pid>/exe, which still runs
    // the original program after a rebuild replaces the file. Empty for a
    // build older than ANTS-5340.
    QString version;
    // Its file was replaced or removed after it started.
    bool replaced = false;
    // Parent, grandparent, ... up to pid 1: how a copy is matched to a tab.
    QList<qint64> ancestors;
};

// This user's running ants-mcpd processes (Linux /proc).
QList<RunningCopy> runningCopies(int timeoutMs);

// True when `copy` is not the build `diskVersion` names: replaced, no
// version line, or a different one.
bool isStale(const RunningCopy &copy, const QString &diskVersion);

}  // namespace mcpd
