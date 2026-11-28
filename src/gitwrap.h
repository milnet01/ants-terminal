// ANTS-1250 — gitwrap: shell-less synchronous git invoker. Wraps
// QProcess::start("git", argv) (the QStringList overload, never the
// single-string form, never via shell-c interpolation) with a
// two-tier wall-clock kill (SIGTERM at kHardKillMs, SIGKILL after
// kKillGraceMs) and a stderr cap of kStderrCapBytes.
//
// The wrapper is intentionally dumb: it does not know about ops or
// JSON. Callers build the argv and parse the output.
//
// See docs/specs/ANTS-1250-git-state-mcp-tool-consolidated.md.

#ifndef ANTS_GITWRAP_H
#define ANTS_GITWRAP_H

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVector>

namespace GitWrap {

// ANTS-4999 — the environment for a read-only git call. A plain
// `git status` refreshes the index and may hold .git/index.lock while it
// does, so a user's `git commit` in that window fails. GIT_OPTIONAL_LOCKS=0
// is git's switch for polling tools: it skips optional locks and still
// takes every lock a write needs. Header-only, so any library can use it.
inline QProcessEnvironment readOnlyEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    return env;
}

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
// ANTS-1839 — default stdout budget. It bounds what one git call can put
// into a JSON reply; a caller expecting more passes its own maxStdoutBytes.
constexpr int kStdoutCapBytes = 1 * 1024 * 1024;  // 1 MiB

// Run `git <argv...>` with cwd, no shell, capped stderr + stdout,
// hard-killed at kHardKillMs. workingDir must already be canonical
// (caller resolves). Pass maxStdoutBytes to widen/narrow the stdout
// budget for a caller with different size expectations.
Result run(const QString &workingDir, const QStringList &argv,
           int maxStdoutBytes = kStdoutCapBytes);

// ANTS-6245 — run() with the caller's own wall budget in place of
// kHardKillMs. cLocale runs git under LC_ALL=C, where `grep -i` folds ASCII
// letters only.
Result runWithin(const QString &workingDir, const QStringList &argv,
                 int maxStdoutBytes, int budgetMs, bool cLocale);

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
    QString            path;    // repo-relative post-image path; pre-image for a deletion
    QVector<DiffHunk>  hunks;
};
QVector<DiffFile> parseDiffHunks(const QByteArray &unifiedDiff,
                                 bool includeLines);

// ANTS-6650 — the path a `--- ` or `+++ ` header line names: git's C-quoting
// decoded, the tab git appends to a name holding a space dropped, and the
// a/ (or b/) prefix stripped. Empty for /dev/null. Callers run git with
// `-c core.quotePath=false` and `--no-ext-diff --src-prefix=a/
// --dst-prefix=b/`, so the user's diff settings cannot change the headers.
QString diffHeaderPath(const QString &line);

// ANTS-5508 — the pure halves of git_state's added-lines diff, log stat and
// rename sweep. Contract: tests/features/git_state_filters/spec.md.

// The `+` lines of a file parsed with includeLines, numbered in the new
// file, marker stripped. A non-empty `match` keeps the lines containing it
// (case-sensitive).
struct AddedLine {
    int     line = 0;
    QString text;
};
QVector<AddedLine> addedLines(const DiffFile &file, const QString &match);

// `git log --numstat` with a format of 0x1e, fields joined by 0x1f, then
// 0x1d. A binary file's counts are -1.
struct NumstatRow {
    QString path;
    int     added   = -1;
    int     removed = -1;
};
struct LogRecord {
    QStringList         fields;
    QVector<NumstatRow> files;
};
QVector<LogRecord> parseLogNumstat(const QByteArray &out);

// `git grep -n -z` output. `rev` is the revision searched, empty for the
// working tree; its `<rev>:` prefix is dropped from each path.
struct GrepHit {
    QString path;
    int     line = 0;
    QString text;
};
QVector<GrepHit> parseGrepZ(const QByteArray &out, const QString &rev);

// The revision a range ends at: B for A..B or A...B, HEAD for A.., and a
// lone revision unchanged.
QString rangeTip(const QString &range);

}  // namespace GitWrap

#endif
