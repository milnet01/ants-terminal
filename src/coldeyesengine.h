// ColdEyesEngine — pure-function helpers for the in-process /cold-eyes
// MCP fold-in (ANTS-1319). Qt::Core only, no widgets.
//
// Five operations:
//
//   derivePartition(projectPath, scope)
//       Discover doc files under <projectPath>, partition into lanes
//       by topic cohesion. Default scope yields:
//         - "contracts": CLAUDE.md + README.md + ROADMAP.md + CHANGELOG.md
//         - "standards": docs/standards/*.md
//         - "decisions": docs/decisions/*.md
//         - "plugins":   PLUGINS.md (if present)
//         - "spec/ANTS-NNNN": one lane per active spec (📋/🚧)
//       Optional override: <projectPath>/.cold-eyes/partition.json.
//       Spec-lane cap = kMaxSpecLanes (12) per ANTS-1319 INV-2.
//
//   assembleBriefManifest(projectPath, lane)
//       Per-lane brief manifest. Paths-only — never inlines doc bodies
//       (ANTS-1319 INV-3, mirrors ANTS-1281 INV-5). Subagent reads
//       each doc itself via its Read tool.
//
//   extractCitedCodePaths(projectPath, docPaths)
//       Regex pass over doc bodies for `src/foo.{h,cpp}` mentions +
//       `file:line` citations. Resolves under projectPath; rejects
//       any path whose canonical form escapes projectPath
//       (ANTS-1319 INV-13).
//
//   crossDocDiffFromDir(projectPath, reportsDirRelative, minLanes, reportsRead)
//       Thin wrapper around IndieReviewEngine::corroboratedFindingsFromDir
//       — same 64 KiB per-report truncate, same `(file, line)` keying.
//       Exists to give cold-eyes a topical entry-point in the MCP
//       namespace (ANTS-1319 INV-5).
//
//   templateColdEyesFoldInBlock(actionable, allocatedIds, dateIso)
//       `### 📝 Cold-eyes <YYYY-MM-DD>` block with one bullet per
//       finding (ANTS-1319 INV-7). Mirrors
//       IndieReviewEngine::templateIndieReviewFoldInBlock.

#pragma once

#include "indiereviewengine.h"  // CorroboratedFinding shared type

#include <QList>
#include <QString>
#include <QStringList>

namespace ColdEyesEngine {

enum class Scope {
    Default,
    DocsOnly,
    ContractsOnly,
};

// INV-2 cap. Surfaced in the response envelope when hit (truncated:true).
constexpr int kMaxSpecLanes = 12;

struct Lane {
    QString     name;
    QString     summary;
    QStringList docPaths;  // project-relative
};

struct PartitionResult {
    QList<Lane> lanes;
    QString     overridePath;   // ".cold-eyes/partition.json" or "<default>"
    Scope       scope = Scope::Default;
    int         scopedCount = 0;  // # of spec lanes considered before INV-2 cap
    bool        truncated = false;  // true if INV-2 cap hit
    // ANTS-1412 — surface malformed `.cold-eyes/partition.json` to the
    // caller. Empty when the override file is absent OR was loaded
    // cleanly. Non-empty when the override was found but rejected
    // (parse error, wrong version, missing lanes array, every lane
    // entry filtered out by INV-13 path-rule defence): partition
    // fell back to the default + caller sees the cause so they can
    // fix the file. Echoed as the `override_warning` field on the
    // MCP `cold_eyes_partition` response.
    QString     overrideWarning;
    // ANTS-1619 — debug field naming which code path built the
    // partition. `"override"` when `.cold-eyes/partition.json`
    // parsed cleanly, otherwise `"default"` (covers the absent +
    // malformed-fall-back cases together, paired with
    // `overrideWarning` for diagnosis).
    QString     partitionSource = QStringLiteral("default");
    // ANTS-1619 — every contract-stem the partition tried. Caller
    // can tell from `discoveredContractFiles[]` vs
    // `missingContractFiles[]` whether the partition silently
    // skipped a doc it documented (the summary-vs-doc_paths gap).
    QStringList discoveredContractFiles;
    QStringList missingContractFiles;
};

struct BriefManifest {
    QString     brief;
    QStringList docPaths;
    QStringList crossReferenceDocs;  // INV-4 fixed contract trio + CHANGELOG
    QStringList citedCodePaths;
    // ANTS-1440 — structured summary surfaced in the MCP envelope.
    // For spec lanes this is the parsed `# ` H1 line of the primary
    // spec (e.g. "ANTS-1435 — session_memory reads honour caller_cwd")
    // instead of the generic lane.summary. Empty when no H1 found
    // — caller falls back to lane.summary as today.
    QString     summary;
};

PartitionResult derivePartition(const QString &projectPath,
                                Scope scope = Scope::Default);

BriefManifest   assembleBriefManifest(const QString &projectPath,
                                      const Lane &lane);

QStringList     extractCitedCodePaths(const QString &projectPath,
                                      const QStringList &docPaths);

// ANTS-1413 — single-doc cross-consistency brief. Cheap entry-point
// for "given a `doc_path`, what other docs in this project should
// it stay consistent with?" Returns the related-doc neighbourhood
// (same-dir siblings, project standards, root contract docs) plus
// a default reviewer-role list — saves callers building a partition
// override and dispatching the full multi-lane brief workflow when
// they just want to sanity-check one fresh spec / doc.
// `docPathRel` must be project-relative and resolve inside
// projectPath (the MCP layer's PathValidation already enforces
// this; the helper trusts the input).
struct SingleDocBrief {
    QString     docPath;
    QString     summary;               // parsed H1 of docPath
    QStringList sameDirSiblings;       // *.md neighbours of docPath
    QStringList standards;             // project standards lane
    QStringList rootContracts;         // CLAUDE/README/ROADMAP/CHANGELOG (+ community docs)
    QStringList recommendedReviewers;  // role labels — orchestrator dispatches one subagent per
};

SingleDocBrief assembleSingleDocBrief(const QString &projectPath,
                                      const QString &docPathRel);

QList<IndieReviewEngine::CorroboratedFinding>
                crossDocDiffFromDir(const QString &projectPath,
                                    const QString &reportsDirRelative,
                                    int minLanes = 2,
                                    int *reportsRead = nullptr);

// ANTS-1509 — in-memory counterpart to crossDocDiffFromDir. Mirrors the
// shape IndieReviewEngine::corroboratedFindings already takes (caller
// passes a {lane: report_text} map; tool computes diff in-memory). The
// `/cold-eyes` skill bundles agent reports inline in the orchestrator's
// context, not on disk — the disk-only variant forced an extra fan-in.
QList<IndieReviewEngine::CorroboratedFinding>
                crossDocDiffFromReports(const QString &projectPath,
                                        const QHash<QString, QString> &reports,
                                        int minLanes = 2);

QString         templateColdEyesFoldInBlock(
                    const QList<IndieReviewEngine::CorroboratedFinding> &actionable,
                    const QList<int> &allocatedIds,
                    const QString &dateIso);

// ANTS-1510 — freeform overload. Renders one bullet per finding without
// a `[PROJ-NNNN]` ID prefix, for projects whose roadmap doesn't use the
// shareable docs/standards/roadmap-format.md § 3.5.1 ID scheme (e.g.
// RetroDB's "Pass N.M" headings). Caller passes the freeform release
// heading separately via `release_block_heading`; this helper emits the
// block body only.
QString         templateColdEyesFoldInBlockFreeform(
                    const QList<IndieReviewEngine::CorroboratedFinding> &actionable,
                    const QString &dateIso);

// String parser for the `scope` MCP arg. Maps "default" / "docs_only"
// / "contracts_only" → Scope enum. Returns false on unknown.
bool            parseScope(const QString &raw, Scope *out);

// Active-spec lookup. Walks ROADMAP.md once, returns the set of
// ANTS-NNNN IDs whose status is 📋 or 🚧. Exposed for testing.
QList<int>      activeSpecIds(const QString &projectPath);

}  // namespace ColdEyesEngine
