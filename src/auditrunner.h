// ANTS-1351 — server-side audit runner. Wraps the existing
// AuditEngine pure-function pipeline (ANTS-1119) + QProcess
// invocation of N external tools (cppcheck, clazy, ruff, bandit,
// semgrep, gitleaks, trivy, shellcheck, mypy) behind a single MCP
// verb. Returns a structured envelope + SARIF file path instead of
// shipping N raw tool outputs through parent context.
//
// Threading model (§ 2.5 of spec): runAudit blocks the caller. The
// MCP dispatcher routes this onto the dedicated m_auditPool worker
// (INV-9) so the GUI thread stays responsive; the worker hosts a
// local QEventLoop that multiplexes the per-tool QProcess signals.
//
// See docs/specs/ANTS-1351.md.

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace AuditRunner {

struct ToolSkip {
    QString tool;
    QString reason;
};

struct RunRequest {
    QString     projectRoot;             // canonical absolute (validated)
    QStringList tools;                   // empty == auto-detect
    QString     scope;                   // "auto" | "files" | "since-tag:<t>" | "branch-diff"
    int         capPerToolSeconds = 30;  // clamped [5, 60]; out-of-range → bad_args
    QString     suppressionsMode;        // "auto" | "none" | "path:<file>"
    QStringList formats;                 // {"sarif"} default; {"sarif","html"} for opt-in
    int         topFindingsCount = 0;    // [0, 100]; out-of-range → bad_args
    // ANTS-1512 — scoped-check mode. `paths` constrains the tool's
    // invocation to a list of project-relative paths (clang-tidy and
    // cppcheck both accept these as positional args). `checks`
    // constrains the tool's enabled check set — currently only
    // clang-tidy honours it (rendered as `--checks=-*,<joined>`);
    // other tools refuse with `bad_args` when `checks` is non-empty.
    // Each path/check is validated through `isAuditArgSafe` /
    // `isAuditCheckSafe` before reaching the child process.
    QStringList paths;
    QStringList checks;
};

struct ToolResult {
    QString    tool;
    QString    status;            // "ok" | "timed_out" | "not_runnable" | "crashed"
    qint64     elapsedMs = 0;
    int        rawCount = 0;
    int        afterFilterCount = 0;
    QJsonArray samples;           // each message ≤ 256 B
};

struct RunResult {
    bool                       ok = true;
    QString                    error;       // non-empty when ok==false
    QString                    code;        // refusal code on failure
    QHash<QString, ToolResult> byTool;
    int                        totalRaw = 0;
    int                        totalActionable = 0;
    int                        noiseRatePct = 0;
    QString                    sarifPath;
    QString                    htmlPath;
    QVector<ToolSkip>          toolsSkipped;
    qint64                     elapsedTotalMs = 0;
    bool                       samplesTruncated = false;
    QJsonArray                 topFindings;  // present iff topFindingsCount > 0
};

// Aggregate cap = min(tools.count * capPerToolSeconds * 1.5, 240 s).
// SIGTERM at per-tool cap; SIGKILL 2 s later. Stderr excerpts capped
// at 256 B and emitted in samples[0].message on crash (INV-4).
RunResult runAudit(const RunRequest &req);

// Pure helpers exposed for the engine test fixtures + the
// envelope-cap cascade (INV-13). Internal use only.
namespace internal {

// Envelope size measurement — implementer uses
// QJsonDocument(env).toJson(Compact).size() inline. Helper here
// for tests so they can replicate the cascade.
qint64 measureEnvelopeBytes(const QJsonArray &samples,
                            const QHash<QString, ToolResult> &byTool);

// Apply the bottom-up sample trim cascade: 10 → 5 → 3. Returns the
// trimmed samples and sets samplesTruncated when any trim fired.
void trimSamplesCascade(QHash<QString, ToolResult> &byTool,
                        bool &samplesTruncated);

// Cap message text to 256 B (UTF-8); appends "…" when truncated.
QString capMessage(const QString &msg);

// ANTS-1446 — compile_commands.json include-path validation.
//
// Walks the JSON at <canonProject>/build/compile_commands.json (or
// the project root as a fallback) for every entry's -I / -isystem /
// -iquote / -include argument; refuses the call if any include path
// escapes the project root AND isn't under a hardcoded system-include
// prefix (/usr/include, /usr/lib, /opt, …). Returns true when the
// file is absent, unreadable, or malformed — those cases let the
// downstream tool surface its own diagnostic.
//
// Helper boundaries:
//   - validateCompileCommands: end-to-end (reads the file).
//   - isIncludePathAllowed:    single-path policy decision.
//   - extractIncludeArgs:      args[] → include-style paths only.
//   - splitCommandString:      shell-style split of the `command`
//                              fallback field when arguments[] absent.
bool validateCompileCommands(const QString &canonProject,
                             QString *errReason);
bool isIncludePathAllowed(const QString &includePath,
                          const QString &entryDir,
                          const QString &projectRoot,
                          QString *reason);
QStringList extractIncludeArgs(const QStringList &args);
QStringList splitCommandString(const QString &cmd);

}  // namespace internal

}  // namespace AuditRunner
