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
#include <QJsonObject>
#include <QPair>
#include <QSet>
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
    // ANTS-1555 — per-project `.audit_cache/` surface. cachePath is
    // the SARIF path under `<root>/.audit_cache/`; empty when the
    // cache write failed (callers should consult `sarifPath` for the
    // /tmp fallback). priorRun is the manifest's pre-existing
    // last_run snapshot before this run was recorded — empty on the
    // first sweep of a project. ANTS-1504 (`since-last-run`) reads
    // priorRun.commit as the diff anchor (a precise findings delta is
    // deferred to the v2 per-finding SARIF parser — see ANTS-1504 § 5).
    QString                    cachePath;
    QJsonObject                priorRun;
    // ANTS-1504 — narrowing-scope surface. `scopeResolved` is the
    // effective scope ("auto" / the requested narrowing scope / "full"
    // when demoted). `scopeAnchorCommit` is the commit diffed against when
    // a narrowing scope ran. `changedFilesCount` is the resolved file-set
    // size. `scopeDemoted`/`scopeDemotedReason` are set only on a
    // stale-cache fallback to a full scan. `noChanges` marks the
    // empty-changeset short-circuit (no tools spawned, no run recorded).
    QString                    scopeResolved;
    QString                    scopeAnchorCommit;
    int                        changedFilesCount = 0;
    QString                    scopeDemoted;
    QString                    scopeDemotedReason;
    bool                       noChanges = false;
    // ANTS-2032 — explicit partiality signal. `partial` is true when a
    // spawned tool did NOT finish cleanly (status != "ok": timed_out /
    // crashed), e.g. one tool blew its per-tool cap or the aggregate cap
    // killed a straggler. `incompleteTools` lists those tool names
    // (sorted). The defensive guarantee the bug (ANTS-2032) asks for is
    // already structural — the runner records every tool's status and
    // returns whatever completed rather than aborting all-or-nothing, and
    // the SARIF artifact is written before the caller serialises the
    // reply — so this just makes the "this run is incomplete" signal a
    // first-class envelope field instead of forcing a by_tool[] scan.
    bool                       partial = false;
    QStringList                incompleteTools;
    // ANTS-1870 — since-last-run findings delta. `delta` carries
    // {added[], removed[], added_count, removed_count,
    // carried_forward_count} and is present only for an actually-narrowed,
    // non-empty `since-last-run` with a readable, untruncated prior
    // baseline. `deltaUnavailableReason` (no_prior_findings /
    // prior_findings_unreadable / findings_truncated) is the mutually
    // exclusive fallback. `findingsTruncated` is the OR over the run's
    // per-tool ParsedOutput.findingsTruncated (a tool hit the
    // kSarifFindingsMax finding ceiling).
    QJsonObject                delta;
    QString                    deltaUnavailableReason;
    bool                       findingsTruncated = false;
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

// ANTS-2032 — names of spawned tools whose status is not "ok"
// (timed_out / crashed), sorted ascending. The run is "partial" iff this
// is non-empty. Pure over the byTool map so the feature test can assert
// the derivation without spawning a real tool.
QStringList incompleteToolNames(const QHash<QString, ToolResult> &byTool);

// Apply the bottom-up sample trim cascade: 10 → 5 → 3. Returns the
// trimmed samples and sets samplesTruncated when any trim fired.
void trimSamplesCascade(QHash<QString, ToolResult> &byTool,
                        bool &samplesTruncated);

// Cap message text to 256 B (UTF-8); appends "…" when truncated.
QString capMessage(const QString &msg);

// ANTS-1820 — test hook for the headless learned-FP suppression path.
// Parses raw tool output applying the fingerprint ledger and returns the
// resulting counts, so the feature test can assert suppression without
// spawning a real tool. rawCount is the tool's raw total (learned FPs
// included); afterFilterCount drops the suppressed total; sampleCount is the
// number of (non-suppressed) samples retained.
struct ParsedCounts {
    int rawCount = 0;
    int afterFilterCount = 0;
    int sampleCount = 0;
    // ANTS-1870 — the full per-finding set (uncapped), whether it hit the
    // kSarifFindingsMax ceiling, and whether every finding carries a
    // 16-hex-char fp. Lets the feature test assert INV-1 without exposing
    // the anonymous-namespace ParsedOutput.
    int  findingsCount = 0;
    bool findingsTruncated = false;
    bool allFindingsHaveHexFp = false;
};
ParsedCounts parseWithSuppression(const QString &tool, const QString &raw,
                                  int sampleCap,
                                  const QSet<QString> &learnedFps);

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

// ANTS-2185 — scoped-positional argv-injection guard. Prefixes `./` to a
// dash-leading relative path so the child tool can't parse it as a flag;
// absolute and ordinary relative paths pass through unchanged.
QString flagSafeScopedPath(const QString &p);

}  // namespace internal

}  // namespace AuditRunner
