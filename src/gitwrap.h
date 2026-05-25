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
    QByteArray stdoutBytes;       // capped per the run() maxStdoutBytes arg
    bool       stdoutTruncated = false;  // ANTS-1839 — stdout hit the cap
    QByteArray stderrTail;  // capped per kStderrCapBytes (INV-7)
};

// Two-tier wall-clock kill (INV-12).
constexpr int kHardKillMs   = 5000;
constexpr int kKillGraceMs  =  200;
// Stderr cap (INV-7).
constexpr int kStderrCapBytes = 4096;
// ANTS-1839 — default stdout budget. Generous enough for the current
// callers (git status / branch / log --oneline, all well under this) but
// bounds a future caller running `git diff` / `log -p`, whose unbounded
// output would otherwise be slurped whole into a JSON envelope.
constexpr int kStdoutCapBytes = 1 * 1024 * 1024;  // 1 MiB

// Run `git <argv...>` with cwd, no shell, capped stderr + stdout,
// hard-killed at kHardKillMs. workingDir must already be canonical
// (caller resolves). Pass maxStdoutBytes to widen/narrow the stdout
// budget for a caller with different size expectations.
Result run(const QString &workingDir, const QStringList &argv,
           int maxStdoutBytes = kStdoutCapBytes);

}  // namespace GitWrap

#endif
