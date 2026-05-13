// DebtSweepEngine — pure-function helpers for the in-process
// /debt-sweep fold (ANTS-1113 v1). Qt::Core only, no widgets.
//
// Eight detectors + scanAll + applyMechanicalFix + two prompt
// templates. See docs/specs/ANTS-1113.md for the full contract.
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
};

// applyMechanicalFix verdict. Lets the MCP handler disambiguate
// "no-op because the file changed under us" from "no-op because
// the detector wasn't fixable" from "IO error" without inspecting
// QFile errno state.
struct ApplyVerdict {
    bool    applied = false;     // true iff the file was mutated
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
ApplyVerdict applyMechanicalFix(
    const QString &projectPath, const Finding &finding);

// Fold-into-roadmap block template per spec § 3.10.
//   Heading:  `### 🧹 Debt-sweep fold-in (<dateIso>)`
//   Bullet:   `- 📋 [ANTS-<id>] **<message>** at <file>:<line>.\n`
//             `  Kind: chore.\n  Source: debt-sweep-<dateIso>.\n`
// Caller pre-allocates IDs via RoadmapFoldIn::allocateIds.
// dateIso MUST be YYYY-MM-DD (no time component).
// Empty input → empty QString.
QString templateDebtSweepFoldInBlock(
    const QList<Finding> &deferred,
    const QList<int> &allocatedIds,
    const QString &dateIso);

// Pure string templating of an LLM triage prompt for the LLM-shaped
// findings (those without a mechanical fix). See spec § 3.11.
QString triagePrompt(const QList<Finding> &llmShaped);

}  // namespace DebtSweepEngine
