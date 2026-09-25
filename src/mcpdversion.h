// ANTS-5340 — which ants-mcpd build Claude Code launches, for Help → About.
//
// Since ANTS-4932 ants-mcpd is rebuilt and reconnected on its own, so its
// build can differ from the terminal's. The About dialog reads it when it
// opens, so a rebuilt ants-mcpd shows with no terminal relaunch.
// Contract: tests/features/mcpd_about_version/spec.md.

#pragma once

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

}  // namespace mcpd
