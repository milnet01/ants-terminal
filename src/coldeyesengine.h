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
//   extractCitedCodePaths(projectPath, docPaths,
//                         staleCitationsOut = nullptr,
//                         citedRegionsOut = nullptr)
//       Regex pass over doc bodies for `src/foo.{h,cpp}` mentions
//       + language-agnostic `file:line` citations (.c/.cpp/.h/.hpp/
//       .py/.ts/.tsx/.js/.jsx/.go/.rs/.lua, ANTS-1633). Resolves
//       under projectPath; rejects any path whose canonical form
//       escapes projectPath (ANTS-1319 INV-13). Returns the paths
//       that resolved on disk; cited-but-missing paths are sorted
//       into `staleCitationsOut` when the caller supplies one (the
//       brief envelope surfaces them as `stale_citations[]`).
//       ANTS-3522 — when `citedRegionsOut` is supplied, the cited
//       LINE numbers from `<path>:<line>` citations are grouped per
//       resolved path (sorted-unique) so a reviewer can read a window
//       around each cited line instead of the whole file.
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
#include <QMap>
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

// ANTS-3740 — per-doc cap on BriefManifest::sectionIndex. 200 covers every
// spec and standard in this corpus with room to spare (the largest is under
// 80 headings); it exists for the contracts lane, whose docPaths include
// CHANGELOG.md — one heading per release plus one per category, thousands of
// them. Bounds the envelope at ~16 KB per doc; a doc that hits it is marked
// `truncated` rather than silently shortened.
constexpr int kMaxSectionsPerDoc = 200;

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

// ANTS-1634(b) — orchestrator-supplied "already fixed in the prior
// loop" record. Loop-2 dispatches inject one of these per item closed
// by loop-1 so the reviewer doesn't re-raise the same finding. Both
// fields are optional individually; an entry with both empty is
// silently dropped at the MCP layer before reaching the engine.
struct PriorLoopFix {
    QString title;
    QString summary;
};

struct BriefManifest {
    QString     brief;
    QStringList docPaths;
    QStringList crossReferenceDocs;  // INV-4 fixed contract trio + CHANGELOG
    // ANTS-3526 — the subset of crossReferenceDocs that are large append-only
    // logs (file size > kLargeCrossRefBytes, e.g. ROADMAP.md / CHANGELOG.md at
    // 1-2 MB / 20-25K lines). A drift-check against a monotonically-growing log
    // is a targeted lookup, never a whole-file read, so the brief lists these
    // under a distinct "SEARCH, do not full-read" section and the Instructions
    // tell the reviewer to grep/read_region them. The ANTS-3521 measurement
    // found these whole-file reads were the #1 /cold-eyes token sink (~856K
    // tok/lane, re-paid lane x loop). Always a subset of crossReferenceDocs.
    QStringList largeCrossReferenceDocs;
    QStringList citedCodePaths;
    // ANTS-1633 — code paths cited in the doc bodies (`<path>:<line>`
    // form, language-agnostic) that the regex matched but the
    // filesystem could not resolve under projectPath. Surfaces as
    // `stale_citations[]` in the brief envelope so per-lane reviewers
    // can flag them as accuracy-dimension findings without redoing
    // the grep + stat themselves.
    QStringList staleCitations;
    // ANTS-3601 — deterministic doc-integrity findings (dead anchors, broken
    // links, TOC gaps) for the lane's OWN docPaths, so the cold-eyes Phase-1e
    // mechanical pre-pass is served ready-made. relDocs = docPaths ONLY (never
    // crossReferenceDocs — the giant ROADMAP/CHANGELOG logs must not be handed
    // to DocIntegrity::check; docs/specs/ANTS-3601.md § 2.7). Surfaces as
    // `doc_integrity[]` in the brief envelope, each entry formatted
    // "<file>:<line>: [<kind>] <message>".
    QStringList docIntegrity;
    // ANTS-3522 — cited code regions. For each RESOLVED cited file that
    // carried a `<path>:<line>` citation, the sorted-unique line numbers the
    // docs cited. Lets a cold-eyes reviewer read a window around each cited
    // line (plus the file's outline) instead of the whole file — the
    // citation-local verification a spec review needs, at a fraction of the
    // bytes. Files cited without a line stay in citedCodePaths only (the
    // reviewer outlines them). Additive; never replaces citedCodePaths.
    QMap<QString, QList<int>> citedCodeRegions;
    // ANTS-1440 — structured summary surfaced in the MCP envelope.
    // For spec lanes this is the parsed `# ` H1 line of the primary
    // spec (e.g. "ANTS-1435 — session_memory reads honour caller_cwd")
    // instead of the generic lane.summary. Empty when no H1 found
    // — caller falls back to lane.summary as today.
    QString     summary;
    // ANTS-3718 — SHA-256 over this lane's FULL review input: the assembled
    // brief text (so a shared-block or Instructions change busts it) followed
    // by the bytes of every docPath and every resolved cited code file. The
    // cold-eyes Phase-5 skip ("this lane's input is byte-identical to the loop
    // where it last passed clean") needs exactly this hash, but the
    // orchestrator cannot compute one without reading everything it is trying
    // to skip — which inverts the saving. The engine already resolves every
    // path, so hashing here costs the caller nothing. Surfaces as
    // `input_hash` in the brief envelope.
    QString     inputHash;
    // ANTS-3740 — per-doc section index for the lane's OWN docPaths: every ATX
    // heading with its slug, level and body span. Asked for by claude-config,
    // whose stated value is NOT diff-review (a cold read still needs the whole
    // document) but that the reviewer gets the map without deriving it and can
    // cite a section anchor instead of a line number — a citation that survives
    // the next edit above it, where a line number does not.
    //
    // docPaths ONLY, never crossReferenceDocs: the same rule ANTS-3601 applies
    // to docIntegrity, and for the same reason — the reviewer is not reviewing
    // the cross-refs, so indexing them buys nothing and ROADMAP.md/CHANGELOG.md
    // would dominate the envelope. `slug` is MarkdownScan::headingSlug, i.e.
    // exactly what `read_region section=` resolves, so the reviewer can fetch
    // any listed section by the slug it was handed.
    //
    // Budget: kMaxSectionsPerDoc entries per doc (~80 bytes each), so a lane
    // is bounded at docPaths.size() × 16 KB regardless of how large its docs
    // are. `truncated` marks a doc whose heading count hit the cap — the
    // contracts lane's CHANGELOG.md has thousands. Deliberately NOT in the
    // brief markdown: the brief is the input_hash's first ingredient, so
    // putting a section index there would bust every lane's existing hash and
    // grow the text the reviewer reads before it reads the docs.
    struct DocSection {
        QString heading;    // heading text, trimmed
        QString slug;       // read_region section= key
        int     level = 0;  // 1..6
        int     startLine = 0;
        int     endLine = 0;
    };
    struct DocSectionIndex {
        QString         path;
        QList<DocSection> sections;
        bool            truncated = false;
    };
    QList<DocSectionIndex> sectionIndex;
};

PartitionResult derivePartition(const QString &projectPath,
                                Scope scope = Scope::Default);

BriefManifest   assembleBriefManifest(const QString &projectPath,
                                      const Lane &lane,
                                      const QList<PriorLoopFix> &priorLoopFixes = {});

QStringList     extractCitedCodePaths(const QString &projectPath,
                                      const QStringList &docPaths,
                                      QStringList *staleCitationsOut = nullptr,
                                      QMap<QString, QList<int>> *citedRegionsOut = nullptr);

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
                    const QString &dateIso,
                    // ANTS-3473 — ID prefix (sniffed from the project's
                    // ROADMAP.md by the caller). Default keeps the Ants
                    // render + existing tests byte-identical.
                    const QString &idPrefix = QStringLiteral("ANTS"));

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
