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
#include <QVector>

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

// ANTS-3377 — parse `git diff --no-color -U<n>` output into per-file
// hunk headers, for splitting a messy working tree into clean commits.
// Pure: takes the diff bytes, forks nothing, returns plain structs (this
// helper stays JSON-free per the header contract above — the MCP caller
// maps it to the git_state envelope). Files carrying no `@@` hunk (a pure
// rename / mode change) are omitted. When `includeLines` is true each hunk
// also carries its raw body lines (context / `+` / `-`, marker included).
struct DiffHunk {
    QString     header;         // full "@@ -a,b +c,d @@ <section>" line
    int         oldStart = 0;   // pre-image start line
    int         oldCount = 1;   // pre-image line span (1 when omitted)
    int         newStart = 0;   // post-image start line
    int         newCount = 1;   // post-image line span (1 when omitted)
    QStringList lines;          // body lines, empty unless includeLines
};
struct DiffFile {
    QString            path;    // repo-relative post-image path
    QVector<DiffHunk>  hunks;
};
QVector<DiffFile> parseDiffHunks(const QByteArray &unifiedDiff,
                                 bool includeLines);

}  // namespace GitWrap

#endif
