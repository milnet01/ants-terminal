// ANTS-1250 — gitwrap: shell-less synchronous git invoker. Wraps
// QProcess::start("git", argv) (the QStringList overload, never the
// single-string form, never via shell-c interpolation) with a
// two-tier wall-clock kill (5 s SIGTERM + 200 ms grace SIGKILL) and
// a 4 KiB stderr cap.
//
// The wrapper is intentionally dumb: it does not know about ops or
// JSON. cmdGitState (in remotecontrol.cpp) builds the argv, calls
// here, and parses output. Same helper is intended for ANTS-1251's
// subsystem.recent_changes per spec §3.
//
// See docs/specs/ANTS-1250.md.

#ifndef ANTS_GITWRAP_H
#define ANTS_GITWRAP_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace GitWrap {

struct Result {
    int        exitCode    = -1;
    bool       started     = false;
    bool       hardKilled  = false;
    bool       crashed     = false;
    QByteArray stdoutBytes;
    QByteArray stderrTail;  // capped per kStderrCapBytes (INV-7)
};

// Two-tier wall-clock kill (INV-12).
constexpr int kHardKillMs   = 5000;
constexpr int kKillGraceMs  =  200;
// Stderr cap (INV-7).
constexpr int kStderrCapBytes = 4096;

// Run `git <argv...>` with cwd, no shell, capped stderr, hard-killed
// at kHardKillMs. workingDir must already be canonical (caller resolves).
Result run(const QString &workingDir, const QStringList &argv);

}  // namespace GitWrap

#endif
