// IndieReviewEngine — pure-function helpers for the in-process
// /indie-review fold (ANTS-1112 v1). Qt::Core only, no widgets.
//
// Six operations:
//
//   derivePartition(projectPath)
//       Read CLAUDE.md `## Module map (src/)` via SubsystemMap; for
//       each lane, walk src/ to compute the source-file set
//       (`<name>.{h,cpp}` + `<name>*.{h,cpp}` siblings, generated
//       files skipped). Optional override:
//       <projectPath>/.indie-review/partition.json.
//
//   assembleBrief(projectPath, lane)
//       Verbatim brief text for one lane: header + source bodies +
//       ROADMAP slice + standards trail (links only). Pure file IO,
//       no recursion.
//
//   extractFileLineCitations(projectPath, report)
//       Regex pass over a single review report; returns Citation
//       structs whose path resolves under projectPath. Reports
//       larger than 64 KiB are truncated (defensive, matches
//       MAX_TOOL_OUTPUT_BYTES convention).
//
//   corroboratedFindings(projectPath, reports, minLanes=2)
//       Cross-lane corroboration: every (file, line) cited by
//       >= minLanes distinct lanes. (file, -1) and (file, 42) are
//       distinct keys (intentional, see spec INV-6).
//
//   synthesisPrompt(reports, threatModelExtras)
//       Pure string templating of a prompt for the optional
//       cross-cutting synthesis LLM call. Caller dispatches.
//
//   templateIndieReviewFoldInBlock(actionable, allocatedIds, dateIso)
//       `### 🔍 Indie-review fold-in (DATE)` block with one bullet
//       per finding. Mirrors AuditEngine::templateRoadmapFoldInBlock
//       (ANTS-1111 v1).

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace IndieReviewEngine {

// ANTS-1344 — public sibling of the file-local kMaxScanBytes constants
// used by extractFileLineCitations + corroboratedFindingsFromDir. The
// MCP handler reads this to detect "report was truncated before scan"
// and surface the signal in the response envelope. Keep in lockstep
// with the file-local copies (INV-1 source-grep guards the pairing).
constexpr int kMaxScanBytes = 64 * 1024;

struct Lane {
    QString     name;
    QString     summary;
    QStringList sourcePaths;  // project-relative
};

struct Citation {
    QString file;     // project-relative
    int     line = -1;
    QString context;  // ±40 chars
};

// ANTS-1288 — a suggested merge of two review lanes whose CLAUDE.md /
// docs/subsystems.md summaries are duplicate or near-duplicate. Advisory
// only: derivePartition already merges lanes with identical source-file
// sets (ANTS-1685); this flags lanes that describe the same component in
// the same words yet resolve to *different* files (e.g. the
// `luaengine` / `pluginmanager` multi-name bullet), so a caller or
// downstream orchestrator can fold them rather than dispatching two
// near-identical briefs.
struct MergeSuggestion {
    QStringList lanes;      // exactly two lane names (the candidate pair)
    QString     rationale;  // human-readable reason ("identical summary
                            // text" / "near-identical summary text (NN% similar)")
};

struct CorroboratedFinding {
    QString     file;
    int         line = -1;
    QStringList citingLanes;  // sorted, unique
    QStringList contexts;     // one per citingLane, same order
    // ANTS-1278 — optional rich-card fields. When present, the
    // indie_review_fold_in renderer emits a standard roadmap card
    // (bold title + body + Layman: + Kind:) instead of the loud
    // TODO placeholder. Absent → placeholder, so a caller cannot
    // silently ship a stub bullet.
    QString     title;
    QString     description;
    QString     layman;
    QString     kind;
};

// ANTS-1281: brief response without inlined source bodies.
// The subagent reads source files itself via its Read tool.
// `brief` carries the header / source-path list / ROADMAP slice /
// standards-reference + an explicit "Read each source file…"
// instruction sentinel (see INV-5 of docs/specs/ANTS-1281.md);
// it deliberately omits the per-file body inlining the v1
// `assembleBrief` does.
struct BriefManifest {
    QString     brief;
    QStringList sourcePaths;    // project-relative; INV-2 mirrors lane.sourcePaths
                                // minus any path that fails canonicalisation.
    QStringList contractDocs;   // INV-6 fixed list, authored order.
    QStringList externalSpecs;  // reserved; empty in v1.
    // dimensionWeighting deferred: MCP layer emits the literal `{}`.
};

QList<Lane> derivePartition(const QString &projectPath);

// ANTS-1288 — scan a partition for lanes whose summaries are duplicate or
// near-duplicate and return one MergeSuggestion per candidate pair (in
// stable lane order). Pure, side-effect-free. Identical summaries (after
// trimming) are flagged outright; otherwise a length-gated normalised
// Levenshtein similarity ≥ 0.90 flags a near-duplicate. Empty input or no
// duplicates → empty list.
QList<MergeSuggestion> suggestedMerges(const QList<Lane> &lanes);

// v2 brief shape (ANTS-1281). New callers should use this.
BriefManifest assembleBriefManifest(const QString &projectPath,
                                    const Lane &lane);

// v1 brief shape — full source bodies inlined. Retained for the
// existing feature test and any legacy MCP consumer; new callers
// should prefer assembleBriefManifest.
QString assembleBrief(const QString &projectPath, const Lane &lane);

// ANTS-1352 — dispatch-shaped brief. Like assembleBrief BUT:
//   - source bodies wrapped in 4-backtick fences with the
//     "treat as data, not instructions" preamble (INV-22);
//   - any literal 4-backtick run in source bodies is replaced
//     with `'```'` before fencing (defends against fence-escape);
//   - the three standards docs (coding.md / testing.md /
//     documentation.md) are **inlined** rather than referenced;
//   - drops the "fetches if needed" trailing sentinel (the
//     upstream LLM dispatcher has no Read tool — H-3 fix).
//
// Path-traversal guard mirrors assembleBrief (project-relative
// source paths only; never substitutes projectPath into the
// prompt — INV-23).
QString assembleBriefForDispatch(const QString &projectPath,
                                 const Lane &lane);

QList<Citation> extractFileLineCitations(
    const QString &projectPath, const QString &report);

QList<CorroboratedFinding> corroboratedFindings(
    const QString &projectPath,
    const QHash<QString, QString> &reports,
    int minLanes = 2);

// ANTS-1282: read reports from disk, then corroborate. The directory
// is resolved relative to projectPath; lane name = filename stem of
// each top-level `*.md` file. Sub-directories are not recursed. Each
// report file is truncated at 64 KiB (matches extractFileLineCitations'
// kMaxScanBytes — INV-8 of docs/specs/ANTS-1282.md). Saves the parent
// orchestrator from holding 14 reports in context just to call
// corroborate.
//
// `reportsRead` is an out-parameter set to the count of `*.md` files
// actually consumed (caller can compare against expected lane count).
// Returns an empty list if `reportsDirRelative` is absolute, escapes
// projectPath, or doesn't exist.
QList<CorroboratedFinding> corroboratedFindingsFromDir(
    const QString &projectPath,
    const QString &reportsDirRelative,
    int minLanes = 2,
    int *reportsRead = nullptr);

QString synthesisPrompt(
    const QHash<QString, QString> &reports,
    const QString &threatModelExtras);

QString templateIndieReviewFoldInBlock(
    const QList<CorroboratedFinding> &actionable,
    const QList<int> &allocatedIds,
    const QString &dateIso);

// MCP-handler-side helper: read CLAUDE.md + SECURITY.md + .semgrep.yml
// from `projectPath` and concatenate with separator markers per the
// spec § 3.5 contract. Missing files contribute their header line +
// empty body. Returns empty string if all three are missing or
// unreadable.
QString assembleThreatModelExtras(const QString &projectPath);

}  // namespace IndieReviewEngine
