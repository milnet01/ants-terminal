#pragma once

// AuditEngine — Qt6::Core-only data types + pure-function helpers
// extracted from auditdialog.cpp per ANTS-1119. The audit dialog is
// a thin presentation layer over this module; non-GUI consumers
// (CI runners, future ants-helper audit-run, MCP server) link
// auditengine.cpp directly without dragging Qt6::Widgets in.
//
// **v1 scope (ANTS-1119 v1):** the pure-data shape (CheckType,
// Severity, OutputFilter, Finding, CheckResult, AuditCheck) plus
// three demonstrably-pure parsing functions (`applyFilter`,
// `parseFindings`, `capFindings`). Subsequent function migrations
// land under follow-up bullets — see ANTS-1119 spec § Open
// questions, § Out of scope.
//
// All types and functions in this header are usable with Qt6::Core
// only; no QWidget, QPainter, QDialog references.

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class ToggleSwitch;  // Forward decl only — declared here for the
                     // AuditCheck.toggle pointer field. The widget
                     // type itself is GUI-side; engine consumers
                     // never dereference it.

// SonarQube-style taxonomy (informational tag for the UI + summary). A single
// check is exactly one of these — the category string (General, Security,
// C/C++, …) is orthogonal and groups checks for display.
enum class CheckType {
    Info,          // Informational counter (line count, large files, …)
    CodeSmell,     // Maintainability concern; not a bug per se
    Bug,           // Code that is demonstrably wrong or likely wrong
    Hotspot,       // Security-sensitive; needs human judgement
    Vulnerability, // Exploitable flaw
};

// SARIF-compatible severity; matches SonarQube's ordering.
enum class Severity {
    Info     = 0,  // FYI, no expected impact
    Minor    = 1,  // Low impact
    Major    = 2,  // Medium impact
    Critical = 3,  // High impact or a security flaw
    Blocker  = 4,  // Severe unintended consequences — fix immediately
};

// Declarative, line-level output filtering. Applied in C++ AFTER the shell
// command returns so filter logic stays in the source instead of buried in
// six-grep pipelines. Keeps individual checks readable and testable.
struct OutputFilter {
    QStringList dropIfContains;
    QString dropIfMatches;
    QStringList keepOnlyIfContains;
    int maxLines = 100;
    QStringList dropIfContextContains{};
    int contextWindow = 5;
};

struct AuditCheck {
    QString id;
    QString name;
    QString description;
    QString category;
    QString command;
    CheckType type = CheckType::CodeSmell;
    Severity severity = Severity::Minor;
    OutputFilter filter;
    bool autoSelect = false;
    bool available = true;
    ToggleSwitch *toggle = nullptr;  // GUI-side; engine never derefs
    std::function<QString(const QString & /*projectPath*/)> inProcessRunner{};
    int timeoutMs = 30000;
};

struct Finding {
    QString checkId;
    QString checkName;
    QString category;
    CheckType type = CheckType::Info;
    Severity severity = Severity::Info;
    QString source;
    QString file;
    int     line = -1;
    QString message;
    QString dedupKey;
    // `highConfidence` survives in 0.7.x as the cross-tool corroboration
    // flag set by `populateChecks` after the second tool reports the
    // same dedup key (auditdialog.cpp:4042). Drives the ★ tag in the
    // summary table, the +20 add in `confidence()` (auditdialog.cpp:2351),
    // and the SARIF property emit. ANTS-1123 indie-review L2 cold-eyes
    // flagged it as potentially zombie — verified live, retained.
    bool    highConfidence = false;
    bool    suppressed = false;
    int     confidence = -1;
    QString snippet;
    int     snippetStart = 0;
    QString blameAuthor;
    QString blameDate;
    QString blameSha;
    QString aiVerdict;
    int     aiConfidence = -1;
    QString aiReasoning;
};

struct CheckResult {
    QString checkId;
    QString checkName;
    QString category;
    CheckType type = CheckType::Info;
    Severity severity = Severity::Info;
    QString source;
    QList<Finding> findings;
    int omittedCount = 0;
    QString output;
    int findingCount = 0;
    bool warning = false;
    // ANTS-1343 — set by a consolidator that authors findingCount
    // out of band (e.g. consolidateMypyStubHints, which collapses N→1
    // and wants findingCount to reflect the pre-collapse N). The
    // dispatcher in auditdialog.cpp gates the post-cap arithmetic
    // overwrite on this flag.
    bool findingCountAuthored = false;
};

namespace AuditEngine {

// FilterResult mirrors the AuditDialog::FilterResult shape (was a
// nested struct before extraction). Returned by applyFilter.
struct FilterResult {
    QString body;
    int count;
};

// Pure-function counterpart of `AuditDialog::applyFilter`. Takes
// `projectPath` so the context-window file lookup (parsing
// `./relative.cpp:LINE:` references in checker output) works without
// member access. Behaviour byte-identical to the prior method —
// ANTS-1119 INV-3 is locked by the audit_rule_fixtures regression
// suite which exercises this path end-to-end.
FilterResult applyFilter(const QString &raw,
                         const OutputFilter &f,
                         const QString &projectPath);

// Pure-function counterpart of `AuditDialog::parseFindings`. Was
// already static; signature is unchanged.
QList<Finding> parseFindings(const QString &body, const AuditCheck &check);

// ANTS-2118 — single source of truth for folding a tool's stdout + stderr
// into the one `raw` string that applyFilter/parseFindings consume. stdout
// is the findings stream; stderr is used alone when stdout is blank (cppcheck
// / clazy / clang-tidy emit findings on stderr) and APPENDED when stdout also
// has content (a tool that splits findings across both channels, e.g.
// clang-tidy under a wrapper). The GUI dialog (auditdialog.cpp) and the
// headless runner (auditrunner.cpp) used to disagree here — the runner dropped
// stderr whenever stdout was non-blank — so a both-channels tool produced a
// different finding set in CI than in the dialog. That is exactly the
// ANTS-1123 silent-divergence class the engine extraction (ANTS-1119) was
// meant to close, migrated up into the un-extracted merge step. Both callers
// now route through this helper for byte-identical input.
QString mergeToolChannels(const QString &stdoutStr, const QString &stderrStr);

// Pure-function counterpart of `AuditDialog::capFindings`. Was
// already static; signature is unchanged.
void capFindings(CheckResult &r, int cap);

// `sourceForCheck` — map a check-id to the canonical "source" tool
// label that gets written to `Finding::source` (and onto SARIF
// `result.runIndex`). One implementation, two callers (engine for
// the parseFindings path, dialog for the per-CheckResult `r.source`
// stamp at populateChecks time). Unifying via the header closes the
// silent-divergence vector ANTS-1123 indie-review C-cluster flagged.
QString sourceForCheck(const QString &checkId);

// Compute a stable 24-hex-char (96-bit) dedup key for a finding.
// Promoted to the public header so a future synthetic-finding site
// (e.g. consolidateMypyStubHints) doesn't have to re-implement it.
QString computeDedup(const QString &file, int line,
                     const QString &checkId, const QString &title);

// ANTS-1262 — confidence score (0-100) for a finding. Pure data-transform
// (reads only Finding fields), moved out of AuditDialog so non-GUI consumers
// (ants-helper, the MCP last_audit_summary verb, headless CI runners) compute
// it without linking Qt6::Widgets or re-implementing the formula (drift risk).
// AuditDialog::computeConfidence forwards here. Weighting: floor +10,
// severity×15, +20 cross-tool corroboration, +10 external AST/semantic tool,
// −20 test path, −5 short grep finding; AI triage caps FALSE_POSITIVE ≤30 /
// TRUE_POSITIVE ≥80; clamped to [0, 100].
int computeConfidence(const Finding &f);

// ANTS-1708 — mark findings whose line-independent content fingerprint
// (ants::auditfp::computeFingerprint) is in the learned-FP ledger set as
// `suppressed = true`, recording `aiReasoning` as a "learned FP" note when it
// is otherwise empty. Mirrors the .audit_suppress mark semantics (surfaces in
// SARIF suppressions[] rather than dropping). Returns the count newly marked.
// Shared by the GUI dialog and the headless audit_run path (ANTS-1706).
int applyLearnedFpSuppressions(QList<Finding> &findings,
                               const QSet<QString> &learnedFingerprints);

// ANTS-1343 — mypy "Library stubs not installed" consolidator. When
// `r.checkId == "mypy"` and ≥ 2 distinct missing stub packages are
// present, collapse the per-package findings into a single synthetic
// Info hint ("N missing stub package(s): pip install …") and stamp
// the pre-collapse count onto `r.findingCount` with
// `findingCountAuthored=true`. No-op for non-mypy checks or single-
// package mypy runs. Pure function — no `this` state required, no GUI
// dependencies. Test access without instantiating AuditDialog.
void consolidateMypyStubHints(CheckResult &r);

// Catastrophic-regex shape detector. Rejects patterns whose AST
// shape is known to backtrack pathologically — quantifier inside a
// quantified group `(.+)+`, alternation under a quantifier
// `(a|b)+`, etc. The detector is conservative; it errs on the side
// of rejecting safe-but-suspicious patterns rather than admitting
// adversarial ones. Used as the first line of defense before
// `hardenUserRegex`'s PCRE2 step-limit.
bool isCatastrophicRegex(const QString &pattern);

// Wrap a user-supplied regex with PCRE2's inline `(*LIMIT_MATCH=N)`
// option so a pattern that slips past `isCatastrophicRegex` still
// has a bounded match-step budget. Empty input → empty output;
// patterns already starting with `(*LIMIT_` → returned unchanged
// (avoids double-prefix); else prepends the canonical
// `(*LIMIT_MATCH=100000)` limit. Single source of truth — both the
// engine's `applyFilter` path and the dialog's `audit_rules.json`
// ingest go through this function.
QString hardenUserRegex(const QString &pattern);

// ANTS-1709 — single source of truth for the directory names every
// audit scanner must skip (VCS, dependency vendoring, language caches,
// our own artifact dirs). `build` is deliberately NOT in this list: it
// expands to a glob family (build, build-asan, build-fast,
// build-workstation, …) and each scanner accepts globs differently, so
// the per-tool formatters below prepend the build family in the right
// syntax. Centralised here so the historical copies (find -not -path,
// grep --exclude-dir, trivy --skip-dirs, cppcheck -i, FeatureCoverage's
// walk) can no longer drift — the drift that let trivy keep scanning
// build-fast / build-workstation after ANTS-1707 fixed the same class
// for cppcheck.
const QStringList &excludedDirNames();

// find(1): a run of `-not -path './<dir>/*'` clauses with the build glob
// family first. `__pycache__` matches at any depth (Python nests it);
// the rest are anchored top-level, preserving the pre-centralisation
// kFindExcl semantics. Leading space; append after the find predicate.
QString findExcludeExpr();

// grep(1): a run of `--exclude-dir=<dir>` flags, build glob first.
// Leading space. `--exclude-dir` is position-safe (unlike file
// `--exclude`, which silently disables a later `--include`).
QString grepExcludeExpr();

// trivy `--skip-dirs`: comma-joined dir list, build glob first. trivy's
// skip-dirs honours shell-style globs, so `build-*` covers every preset.
QString trivySkipDirsCsv();

// cppcheck `-i`: cppcheck can't glob a -i prefix, so emit a shell
// snippet that expands `build*` at run time and prints one `-i <dir>`
// per existing match. Leading space; embed inside the cppcheck command.
QString cppcheckIgnoreShellExpr();

// ANTS-1254 — wire-shape view of a single SARIF finding for the
// last_audit_summary MCP tool. Distinct from `Finding` (the audit
// dialog's in-memory parse target) because the wire needs the
// SARIF `level` string AND the resolved 5-level severity string,
// neither of which `Finding` carries (it has only the `Severity`
// enum).
struct AuditSummaryFinding {
    QString level;            // "error" / "warning" / "note"
    QString severity;         // BLOCKER/CRITICAL/MAJOR/MINOR/INFO
    QString ruleId;
    QString file;             // SARIF artifactLocation.uri (as-is; INV-7)
    int     line           = 0;
    QString message;
    int     confidence     = -1;
    bool    highConfidence = false;
};

struct AuditSummary {
    QString sarifPath;
    QString htmlPath;         // "" if no sibling within ±60 s
    QString runAtIso;         // empty if SARIF lacks invocations
    int     countError      = 0;
    int     countWarning    = 0;
    int     countNote       = 0;
    int     countSuppressed = 0;
    QList<AuditSummaryFinding> topFindings;
    // ANTS-1459 — echoes the source format the summary was parsed
    // from. "sarif" for the SARIF path, "cppcheck-xml" for the
    // native cppcheck XML fallback. Empty when set by an older
    // call site (defaults to "sarif" in the envelope when blank).
    QString sourceFormat;
    // ANTS-1539 — capture-time git provenance, scraped from SARIF
    // run.versionControlProvenance (§ 3.14.18). Empty when the SARIF
    // omits the block or the source format doesn't carry it (every
    // non-SARIF format today — they have no equivalent surface). The
    // envelope-builder omits the fields when empty so callers can
    // distinguish "unknown" from "main" / "abc123".
    QString branch;        // versionControlDetails.branch
    QString commit;        // versionControlDetails.revisionId
    QString repositoryUri; // versionControlDetails.repositoryUri
    // ANTS-1576 — provenance origin tag. Set by cmdLastAuditSummary
    // after parser-side scrape: "file_provenance" when branch/commit
    // arrived from the source file's metadata, "read_time" when the
    // handler back-filled from a live `git rev-parse` against the
    // project root, "" when no provenance is available.
    QString branchSource;
};

// ANTS-1254 — read SARIF at `sarifPath`, return compact summary.
// Caller decides path discovery + caching. Pure parser: no Qt6::Widgets,
// no I/O outside the read.
//   `topN`       — server-clamped by caller (this fn trusts the input).
//   `levelFloor` ∈ {"error","warning","note"} (caller-validated).
// Returns nullopt on read or parse failure (caller distinguishes by
// checking QFile::exists() before calling).
std::optional<AuditSummary> summariseSarif(
    const QString &sarifPath,
    int topN,
    const QString &levelFloor);

// ANTS-1459 — parse cppcheck's native XML output (--xml --xml-version=2)
// and return the same AuditSummary shape so last_audit_summary callers
// don't have to branch on source format. Discovered separately from
// SARIF; the result struct carries `sarifPath` set to the XML path
// for cache-key consistency (the field is misnamed historically;
// rename out of scope for ANTS-1459).
// Severity mapping:
//   cppcheck "error"        → level="error"   severity="MAJOR"
//   cppcheck "warning"      → level="warning" severity="MAJOR"
//   "style"/"performance"/
//   "portability"           → level="note"    severity="MINOR"
//   "information"           → level="note"    severity="INFO"
// Returns nullopt on read or malformed XML (caller distinguishes by
// checking QFile::exists() before calling).
std::optional<AuditSummary> summariseCppcheckXml(
    const QString &xmlPath,
    int topN,
    const QString &levelFloor);

// ANTS-1494 — parse clang-tidy's native text output (one finding per
// line, `file.cpp:LINE:COL: warning|error|note: message [check-name]`).
// Map clang-tidy severities to SARIF levels 1:1 (warning→warning,
// error→error, note→note). topN + levelFloor mirror summariseSarif.
// Returns nullopt on read failure or zero parseable findings.
std::optional<AuditSummary> summariseClangTidyText(
    const QString &textPath,
    int topN,
    const QString &levelFloor);

// ANTS-1494 — parse semgrep's native JSON output (`semgrep --json`).
// Each finding sits under `results[]` with `path`, `start.line`,
// `check_id`, and `extra.severity` ∈ {ERROR, WARNING, INFO}. Mapped to
// SARIF levels error|warning|note. Returns nullopt on read failure or
// malformed JSON.
// ANTS-2123 — a `# nosemgrep`-ignored finding (extra.is_ignored=true) is
// tallied into countSuppressed in parallel (not excluded), matching the SARIF
// suppressions[] path (ANTS-1254 INV-3).
std::optional<AuditSummary> summariseSemgrepJson(
    const QString &jsonPath,
    int topN,
    const QString &levelFloor);

// ANTS-1576 — capture best-effort VCS state for the project at
// `rootCanonical` (canonical, symlink-resolved). Returns a SARIF
// versionControlProvenance array (§ 3.14.18) suitable for
// `run["versionControlProvenance"] = ...`. Empty array when
// `git rev-parse HEAD` returns empty (non-git tree, missing binary,
// canonical path empty) — callers omit the block in that case.
// Forks at most three git subprocesses (rev-parse, symbolic-ref,
// remote.origin.url), each with a 2 s wall-clock cap.
QJsonArray buildVcsProvenanceBlock(const QString &rootCanonical);

// ANTS-1111 — severity-tier shift on cross-tool corroboration. In-place
// mutation of `findings[].severity`:
//   coverageCount(f) = number of distinct CheckIds whose findings cite
//                      the same (f.file, f.line) pair.
//   shift = +1 if coverageCount >= 2     (clamp to Blocker)
//   shift = -1 if coverageCount == 1 AND f.checkId in noisyRules
//                                        (clamp to Info)
//   shift =  0 otherwise
// The +20 confidence-score bump in computeConfidence is unchanged —
// this complements rather than replaces it.
void applyCorroborationShift(QList<Finding> &findings,
                             const QSet<QString> &noisyRules);

// ANTS-1111 — render a single ROADMAP fold-in subsection block, ready
// to splice into ROADMAP.md by RoadmapFoldIn::insertBlock. Subsection
// shape per roadmap-format.md § 3.8; per-bullet fields per § 3.5.
//   `actionable`     — findings the user confirmed as actionable.
//   `allocatedIds`   — pre-allocated `[ANTS-N]` IDs (size must equal
//                      actionable.size()).
//   `dateIso`        — YYYY-MM-DD for the heading + Source line.
// Output begins with `### 🔍 Audit fold-in (DATE)`; each finding maps
// to one bullet `- 📋 [ANTS-N] **MESSAGE.** at FILE:LINE (rule).
//   Kind: audit-fix.
//   Source: audit-DATE.
//   Lanes: <derived from finding.file>.`
// Empty input -> empty string.
QString templateRoadmapFoldInBlock(const QList<Finding> &actionable,
                                   const QList<int> &allocatedIds,
                                   const QString &dateIso);

}  // namespace AuditEngine
