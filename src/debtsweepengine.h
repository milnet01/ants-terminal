// DebtSweepEngine — pure-function helpers for the in-process
// /debt-sweep fold (ANTS-1113 v1; detector set expanded ANTS-1358).
// Qt::Core only, no widgets.
//
// 11 detectors + scanAll + applyMechanicalFix + two prompt
// templates. See docs/specs/ANTS-1113.md for the v1 contract and
// docs/specs/ANTS-1358.md for the detector-expansion addendum.
//
// All file IO is constrained to <projectPath>; all git invocations
// run via QProcess with stdout caps + 30 s timeout.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace DebtSweepEngine {

// One detector finding. category is one of the four canonical
// strings: "code_drift" | "test_coverage" | "doc_drift" |
// "packaging_drift". detectorId is a stable identifier (e.g.
// "stale_type_comment", "orphan_q_unused"). autoFixable is true
// iff applyMechanicalFix can land the suggested edit without
// asking.
struct Finding {
    QString category;
    QString detectorId;
    QString file;          // project-relative
    int     line = -1;     // 1-based; -1 for file-level
    QString message;
    QString suggestedFix;
    bool    autoFixable = false;
};

// Scope: which commit range to inspect for "added in scope"
// detectors. Empty sinceRef → auto-detect (`git describe --tags
// --abbrev=0`; `HEAD~10` if no tags). Detectors that don't need a
// baseline (orphan Q_UNUSED, ROADMAP commit cross-check) ignore
// sinceRef.
struct ScanOptions {
    QString sinceRef;
    bool    includeCodeDrift = true;
    bool    includeTestCoverage = true;
    bool    includeDocDrift = true;
    bool    includePackagingDrift = true;
    // detectStaleTodos threshold: a TODO/FIXME whose git-blame
    // committer-time is older than this many days is flagged. 0 or
    // negative disables the detector (treated as "no stale floor").
    int     staleTodoMaxAgeDays = 180;
};

// applyMechanicalFix verdict. Lets the MCP handler disambiguate
// "no-op because the file changed under us" from "no-op because
// the detector wasn't fixable" from "IO error" without inspecting
// QFile errno state.
struct ApplyVerdict {
    bool    applied = false;     // true iff the file was mutated
    // ANTS-2227 — dry_run preview: true iff the fix passed every guard
    // and a patched body was computed, but the write was skipped because
    // dryRun was requested. `applied` stays false (nothing was mutated).
    bool    wouldApply = false;
    QString errorCode;           // "" on success or expected no-op;
                                 // "not_fixable" / "file_changed" /
                                 // "io_error" on failure
    QString errorMessage;        // human-readable detail; empty on success
};

// ---------------------------------------------------------------------------
// Per-category detectors. Each is a pure function; combined by
// scanAll() but exposed individually for invariant tests + future
// selective dispatch.
// ---------------------------------------------------------------------------

// Code drift (a) — comments naming a type/symbol absent from the
// project source blob. Walks `git diff <sinceRef>..HEAD --name-only
// -- '*.cpp' '*.h' '*.py' '*.js' '*.ts' '*.tsx'`; extracts comments
// from those files; for each leading-cap CamelCase token (regex:
// `\b([A-Z][A-Za-z0-9_]{3,})\b`, ≥4 chars) checks the project
// source blob via FeatureCoverage::existsInSource. Drops tokens in
// FeatureCoverage::specStopwords().
QList<Finding> detectStaleTypeComments(
    const QString &projectPath, const ScanOptions &opt);

// Code drift (c) — TODO / FIXME markers added in scope. Diffs
// <sinceRef>..HEAD; reports any new line containing
// `\b(TODO|FIXME|XXX|HACK)\b:`. Excludes NOTE: + the file's own
// copyright block.
QList<Finding> detectAddedTodos(
    const QString &projectPath, const ScanOptions &opt);

// Code drift (d) — Q_UNUSED(x) / (void)x; markers wrapping an
// undeclared variable. Per-file pass: extract marker arg `x`,
// then check the same file for any declaration of `x`. Reports
// markers whose target isn't declared anywhere in the file.
// autoFixable = true (mechanical: delete the line).
QList<Finding> detectOrphanQUnused(
    const QString &projectPath, const ScanOptions &opt);

// Test coverage gap — INV-N markers in
// tests/features/*/spec.md without a matching INV-N reference in
// the corresponding test_*.{cpp,py,js,go,rs}. INV-id regex is
// `\bINV-([0-9][0-9a-zA-Z]*)\b` (numeric leader + optional
// alphanumeric tail — covers INV-7 and INV-8b).
QList<Finding> detectMissingInvariantTests(
    const QString &projectPath, const ScanOptions &opt);

// Doc drift (a) — ROADMAP ✅ items whose stable ID isn't mentioned
// in any commit subject. Reads ROADMAP.md, extracts
// `^- ✅ \[(ANTS-\d+)\]`; runs `git log --all --format=%s` ONCE;
// reports IDs with zero subject mentions. Self-disables on
// non-git checkouts.
QList<Finding> detectRoadmapShippedWithoutCommit(
    const QString &projectPath, const ScanOptions &opt);

// Doc drift (b) — CHANGELOG `[Unreleased]` bullets that reference
// a file path not present in `git diff <sinceRef>..HEAD --name-only`.
// Self-disables if CHANGELOG.md has no `[Unreleased]` section.
QList<Finding> detectChangelogStaleBullets(
    const QString &projectPath, const ScanOptions &opt);

// Code drift (e) — TODO/FIXME markers that have outlived
// `opt.staleTodoMaxAgeDays` (default 180). Distinct from
// detectAddedTodos (which is scope/diff based): this flags
// *long-lived* markers via `git blame --line-porcelain`
// committer-time. Self-disables on non-git checkouts and when
// staleTodoMaxAgeDays <= 0. Flag-only (NOT autoFixable) — a stale
// TODO needs human judgement, not a mechanical delete.
//
// NOTE (ANTS-1358): the roadmap floated a "TODO older than N
// commits" measure; we use committer-time age in days because git
// blame surfaces it directly and per-file commit-distance counting
// is both ambiguous (commits touch files at different rates) and
// O(unique-sha) extra git calls. Same intent, deterministic source.
QList<Finding> detectStaleTodos(
    const QString &projectPath, const ScanOptions &opt);

// Code drift (f) — the same header `#include`d two or more times in
// one file. Reports the second and later occurrences. autoFixable =
// true (mechanical: delete the redundant include line).
//
// NOTE (ANTS-1358): the roadmap floated a cppcheck-driven
// "unused_include" detector, but cppcheck has no unused-include
// check (only unusedVariable / unusedFunction / unusedStructMember)
// and include-what-you-use is not a project dependency. A true
// unused-include analysis needs a compiler frontend; the redundant-
// duplicate case is the deterministic, dependency-free, safely
// auto-fixable subset of the same intent.
QList<Finding> detectDuplicateIncludes(
    const QString &projectPath, const ScanOptions &opt);

// Code drift (g) — removed-in-Qt6 QString idioms that carry an
// unambiguous mechanical replacement (`QString::null` → `QString()`,
// `.toAscii()` → `.toLatin1()`, `.fromAscii(` → `.fromLatin1(`).
// autoFixable = true.
//
// NOTE (ANTS-1358): the roadmap floated a clazy-driven
// "obsolete_qstring_arg" detector. clazy-standalone needs a
// compile_commands.json + resolved include paths to run, and the
// clazy `qstring-arg` combine-fixit is not unconditionally safe
// (arg-index reuse). This detector covers the removed-API subset
// whose replacement is byte-deterministic, with no external tool.
QList<Finding> detectObsoleteQStringIdioms(
    const QString &projectPath, const ScanOptions &opt);

// Code drift (h) — a statement immediately following an
// unconditional control-flow exit (`return …;` / `break;` /
// `continue;` / `throw …;`) within the same block, before the
// closing brace. Conservative line-local heuristic. Flag-only
// (NOT autoFixable): deleting "dead" code is unsafe — macros,
// `[[fallthrough]]`, and multi-line constructs can make the
// following statement reachable. Surface for human triage.
QList<Finding> detectDeadBranchAfterReturn(
    const QString &projectPath, const ScanOptions &opt);

// Pure per-file scan cores for the content-scanning detectors above.
// The detectors enumerate git-tracked *.cpp/*.h and delegate to
// these; exposed so invariant tests can drive the logic against an
// in-memory body without a git fixture. `relPath` is echoed verbatim
// into Finding.file.
namespace detail {
QList<Finding> scanDuplicateIncludes(const QString &relPath, const QString &body);
QList<Finding> scanObsoleteQStringIdioms(const QString &relPath, const QString &body);
QList<Finding> scanDeadBranchAfterReturn(const QString &relPath, const QString &body);
}  // namespace detail

// Packaging drift — wraps `bash packaging/check-version-drift.sh`.
// Parses `<file>:<line>: <label> version <got> drifts from
// CMakeLists.txt <truth>` stdout into Finding structs. Self-disables
// silently when the script is absent or non-executable. NOT marked
// autoFixable.
QList<Finding> runPackagingDrift(
    const QString &projectPath, const ScanOptions &opt);

// Convenience: run every enabled detector and concatenate. Order
// is the canonical category order (code, test, doc, packaging).
// Within each category, detectors run in declaration order.
QList<Finding> scanAll(
    const QString &projectPath, const ScanOptions &opt);

// Apply ONE mechanical fix in-place. See § 3.9 of the spec for
// the verdict-state machine. Caller is responsible for re-running
// the build after applying fixes.
//
// ANTS-2227 — when `dryRun` is true, every guard runs and the patched
// body is computed, but the file is NOT written: the verdict carries
// `wouldApply=true` (and `applied=false`) so the caller can preview
// that the fix is live without mutating source.
ApplyVerdict applyMechanicalFix(
    const QString &projectPath, const Finding &finding,
    bool dryRun = false);

// Fold-into-roadmap block template per spec § 3.10.
//   Heading:  `### 🧹 Debt-sweep fold-in (<dateIso>)`
//   Bullet:   `- 📋 [<prefix>-<id>] **<message>** at <file>:<line>.\n`
//             `  Kind: chore.\n  Source: debt-sweep-<dateIso>.\n`
// Caller pre-allocates IDs via RoadmapFoldIn::allocateIds and passes the
// project's sniffed prefix (RoadmapFoldIn::sniffIdPrefix) so the block
// carries the project's own ID scheme, zero-padded — NOT a hardcoded
// `ANTS` (ANTS-3497, mirrors the ANTS-3473/3480 fold-in treatment).
// dateIso MUST be YYYY-MM-DD (no time component).
// Empty input → empty QString.
QString templateDebtSweepFoldInBlock(
    const QList<Finding> &deferred,
    const QList<int> &allocatedIds,
    const QString &dateIso,
    const QString &idPrefix);

// Pure string templating of an LLM triage prompt for the LLM-shaped
// findings (those without a mechanical fix). See spec § 3.11.
QString triagePrompt(const QList<Finding> &llmShaped);

// ANTS-3346 — bulk-defer triage gate. The Project Audit "Defer all to ROADMAP"
// button (and the debt_sweep_defer MCP verb) once folded a whole 700-1100
// finding scan straight into ROADMAP.md as tracked items, burning that many
// counter IDs on un-reviewed false positives. The gate fires on the TOTAL
// deferred count: a raw scan mixes real, FP-prone, and mechanical findings, so
// bulk size — not auto-fixability — is the honest "this is un-reviewed scan
// output" signal. This pure predicate lets both callers refuse (MCP) / confirm
// (GUI) such a batch unless the caller asserts `triaged`. No IO — tests cleanly.
constexpr int kBulkDeferTriageThreshold = 25;

struct TriageGateVerdict {
    bool    allowed = true;          // false → gate the defer (refuse / confirm)
    int     total = 0;               // total deferred findings (the gate signal)
    int     nonAutoFixable = 0;      // judgment-required subset (message only)
    int     threshold = 0;           // kBulkDeferTriageThreshold, echoed back
    QString reason;                  // human-readable, set only when blocked
};

// `triaged` is the caller's explicit assertion that the batch was reviewed
// (always allowed). Otherwise a batch whose total size exceeds
// kBulkDeferTriageThreshold is gated.
TriageGateVerdict evaluateTriageGate(const QList<Finding> &deferred,
                                     bool triaged);

}  // namespace DebtSweepEngine
