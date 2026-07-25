// ANTS-1351 — server-side audit runner. Wraps the existing
// AuditEngine pure-function pipeline (ANTS-1119) + QProcess
// invocation of N external tools (cppcheck, clazy, clang-tidy, ruff,
// bandit, semgrep, gitleaks, trivy, shellcheck, mypy — kKnownTools())
// behind a single MCP verb. Returns a structured envelope + SARIF file
// path instead of shipping N raw tool outputs through parent context.
//
// Threading model (INV-9): runAudit blocks its calling thread and hosts
// a local QEventLoop that multiplexes the per-tool QProcess signals, so
// it must never run on the GUI/MCP thread (ANTS-2103). Both MCP dispatch
// paths honour that with an ad-hoc QThread::create() — the sync verb
// joins it, the async verb (ANTS-3396) hands it to a job registry. There
// is no `m_auditPool` thread pool: that was an unbuilt v2 design, and
// ANTS-3612 replaced it with an engine-side aggregate concurrency cap
// (kMaxConcurrentRuns in the .cpp) that refuses with `server_busy`.
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

// ── ANTS-1351-INV-1 / ANTS-3585 — aggregate wall-clock ceiling for one
// runAudit() call. Raised from 240'000 so a per-tool cap up to
// kCapPerToolMax (300 s) is not immediately re-clamped by the aggregate
// ceiling; only binds on an opt-in high cap (the default 30 s run is
// nowhere near it) and is meant for the async path, which survives the
// MCP transport timeout.
//
// ANTS-3611 — PUBLIC (was an auditrunner.cpp-local constant) because the
// MCP layer's stale-slot reapers must track it: a legitimately-running
// audit may occupy its in-flight / job slot for up to this long, so any
// reap window keyed to a smaller hardcoded number would free the slot
// under a live worker and let a second run start concurrently. Derive,
// never re-hardcode (see ClaudeIntegration::kVerbInFlightReapMs).
inline constexpr int kAggregateCapMs = 900'000;

struct ToolSkip {
    QString tool;
    QString reason;
};

struct RunRequest {
    // Raw caller_cwd as passed by the MCP layer. runAudit canonicalises
    // it into a local (`canonProject`) and validates that — this field is
    // NOT rewritten. See docs/specs/ANTS-1351.md INV-2.
    QString     projectRoot;
    QStringList tools;                   // empty == auto-detect
    // "auto" (whole tree) | "full" (ANTS-2015, explicit whole tree) |
    // "files" | "branch-diff" | "since-tag:<t>" | "since-last-run"
    // (ANTS-1870). A narrowing scope with no resolvable changed-file
    // set demotes to a full scan (scopeDemoted/-Reason below).
    QString     scope;
    int         capPerToolSeconds = 30;  // range [5, 300]; out-of-range →
                                         // bad_args (never clamped)
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
    // "ok" | "timed_out" | "crashed" — the only three states the runner
    // ever sets. A tool that could not be resolved on PATH never gets a
    // ToolResult at all; it lands in RunResult::toolsSkipped instead.
    QString    status;
    qint64     elapsedMs = 0;
    int        rawCount = 0;
    int        afterFilterCount = 0;
    QJsonArray samples;           // each message ≤ 256 B
    // ANTS-3585 — source files this tool failed to PARSE (cppcheck syntaxError
    // / internalError / … on a TU its frontend can't handle, e.g. C++23). A
    // file here got ZERO coverage; empty for non-cppcheck / clean runs.
    QStringList parseFailureFiles;
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
    // ANTS-3585 — richer partiality surface. `incompleteToolsDetail` is one
    // {tool, status, elapsed_ms, truncated} object per non-ok tool (truncated
    // == status=="timed_out"), so a caller can tell a cut-off from a crash
    // without scanning by_tool[] (which the async-poll envelope doesn't carry).
    // `parseFailures` is the deduped union of every tool's parseFailureFiles —
    // source files that got ZERO coverage because the tool couldn't parse them.
    QJsonArray                 incompleteToolsDetail;
    QStringList                parseFailures;
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

// Aggregate cap = min(tools.count * capPerToolSeconds * 1.5,
// kAggregateCapMs). SIGTERM at per-tool cap; SIGKILL 2 s later. On a
// crash, a stderr excerpt capped at 256 B is emitted in
// samples[0].message on a BEST-EFFORT basis (INV-4): a crash with no
// stderr, or stderr the parser can't shape into a sample, yields an
// empty samples[] — the "crashed" status is the reliable signal.
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

// ANTS-3585 — per-tool incompleteness detail: one {tool, status, elapsed_ms,
// truncated} object for every tool whose status is not "ok", sorted by tool
// name (truncated == status=="timed_out"). Empty when the run is complete.
// Pure over byTool so the feature test can assert the derivation.
QJsonArray incompleteToolsDetail(const QHash<QString, ToolResult> &byTool);

// ANTS-3585 — deduped, ascending union of every tool's parseFailureFiles.
QStringList parseFailureFiles(const QHash<QString, ToolResult> &byTool);

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
    // ANTS-3395 — true when a JSON tool logged a fatal abort and produced no
    // parseable findings (the runner promotes this to a "crashed" status).
    bool aborted = false;
    // ANTS-3585 — source files whose parse failed (cppcheck syntaxError /
    // internalError / internalAstError / preprocessorErrorDirective /
    // cppcheckError). A TU here got zero real coverage. Empty for
    // non-cppcheck tools and for clean runs.
    QStringList parseFailureFiles;
};
// ANTS-3615 — `allowlistPath` is optional; when non-empty the file is
// loaded through AuditEngine::loadAllowlist and its entries suppress
// alongside the fingerprint ledger, so a test can assert the headless
// `.audit_allowlist.json` filter without spawning a tool.
ParsedCounts parseWithSuppression(const QString &tool, const QString &raw,
                                  int sampleCap,
                                  const QSet<QString> &learnedFps,
                                  const QString &allowlistPath = {});

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

// ANTS-3629 — test hook for the DOCUMENT-wide SARIF ceiling. Writes a SARIF
// at `path` from `findingsByTool` and sets *docTruncated when the cross-tool
// kSarifFindingsMax ceiling shed any result. Exposed because the overflow it
// reports is otherwise only reachable by spawning enough real tools to sum
// past 10 000 findings — the per-tool caps never fire in that scenario,
// which is exactly why it went unreported.
bool writeSarifForTest(const QString &path,
                       const QHash<QString, ToolResult> &byTool,
                       const QHash<QString, QJsonArray> &findingsByTool,
                       bool *docTruncated);

// ANTS-2185 — scoped-positional argv-injection guard. Prefixes `./` to a
// dash-leading relative path so the child tool can't parse it as a flag;
// absolute and ordinary relative paths pass through unchanged.
QString flagSafeScopedPath(const QString &p);

// ANTS-3394 — test hook exposing the per-tool argv builder so the default
// exclusion-set wiring can be asserted without spawning a real tool.
// `scopedPaths` empty == the whole-tree invocation (exclusions applied);
// non-empty == a scoped invocation (exclusions deliberately omitted).
QStringList toolArgv(const QString &tool, const QString &projectRoot,
                     const QStringList &scopedPaths = {});

}  // namespace internal

}  // namespace AuditRunner
