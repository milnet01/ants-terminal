// ANTS-1961 / ANTS-1962 — shared pure module for the cross-session
// `*_Ants_MCP_Feedback.md` files. Qt6::Core-only, lives in
// ants_core_lib so it is unit-testable without RemoteControl /
// MainWindow (mirrors readlog.h). The thin cmdFeedbackQuery /
// cmdFeedbackLog wrappers (remotecontrol.cpp) handle path resolution +
// PathValidation + the suffix guard + the caller_cwd contract, then
// call into the helpers here.
//
// Format contract: docs/standards/mcp-feedback-files.md
// Read verb spec:  docs/specs/ANTS-1961.md (§ 2.2 is the canonical home
//                  declaring the full namespace surface).
// Write verb spec: docs/specs/ANTS-1962.md (§ 2).

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace FeedbackFile {

// ---- ANTS-1961: read side (the un-triaged delta parser) -------------

// A maintainer tracking-table row. Authored by the write side (ANTS-1962,
// renderTrackingBlock) and re-surfaced on the read side (ANTS-3371,
// ParseResult::trackingRows). Defined before ParseResult so the read
// parser can carry the structured rows.
struct TrackingRow {
    QString     item;
    QStringList ids;     // ANTS-NNNN strings; empty → renders `n/a`
    QString     status;  // an emoji from the machine-readable set
    QString     notes;   // optional; triggers the 4th column when any row has it
    int         line = -1;  // ANTS-3442: 1-based source line of this data row
                            // (populated by parse(); used by pruneTracking to
                            // locate the row for removal)
};

struct ParseResult {
    QString     delta;               // text from the first contributor heading
                                     // after the last maintainer heading to EOF
    bool        deltaPresent = false;
    int         deltaStartLine = -1; // 1-based line of the FIRST contributor
                                     // heading of the delta; -1 when empty
    int         deltaLineCount = 0;  // lines in the full (pre-cap) delta
    QStringList mappedIds;           // unique, sorted; ANTS-[0-9]+ from
                                     // maintainer-block bodies only
    int         maintainerBlockCount = 0;
    int         lastMaintainerLine = -1; // 1-based; -1 when none
    QVector<TrackingRow> trackingRows;   // ANTS-3371: every maintainer
                                         // tracking-table data row, in
                                         // document order (later rows
                                         // supersede earlier ones for the
                                         // same ID). Surfaced only when
                                         // feedback_query is called with
                                         // include_tracking.
};

// Parse the full file content per mcp-feedback-files.md § "The
// un-triaged delta (parser contract)": fenced regions skipped, only
// `^# `/`^## ` (one or two hashes) outside fences are boundaries,
// maintainer headings identified by the anchor regex.
ParseResult parse(const QString &fileContent);

// ---- ANTS-1962: write side (block renderers) ------------------------

struct Finding {
    QString title;
    QString what;
    QString repro;
    QString impact;
    QString suggestedFix;
};

// Render a contributor session block: a dated heading (`## ` default,
// `# ` when h1Heading) + optional note prose + one `### <title>`
// sub-block per finding (only provided field bullets emitted; the
// `**Proposed ID:**` line is always emitted blank).
QString renderFindingBlock(const QString &date, const QString &sessionLabel,
                           bool h1Heading, const QString &note,
                           const QVector<Finding> &findings);

// Render a maintainer tracking block: the fully-qualified watermark
// heading `## 📋 Ants Terminal roadmap tracking update (<date>,
// maintainer)` + optional note + the mapping table (+ optional sentinel
// breadcrumb). A 4th `Notes` column is added iff any row carries notes.
QString renderTrackingBlock(const QString &date, const QString &note,
                            const QVector<TrackingRow> &rows, bool sentinel);

// Render the file skeleton for an absent file: the marker comment + a
// derived `# <projectTitle> …` H1 + the contributor-pointer blockquote.
QString skeleton(const QString &projectTitle);

// ---- ANTS-3421: maintainer compaction (compact_shipped) -------------
//
// Collapse a confirmed-shipped contributor finding write-up to a
// one-line stub, keeping the boundary heading verbatim so the parser's
// maintainer/contributor classification is unchanged. See
// docs/specs/ANTS-3421.md for the full contract (gates + invariants).

struct CompactTarget {
    QString heading;         // verbatim boundary heading (trailing ws trimmed)
    int     headingLine = -1;// optional 1-based locator / disambiguator
    QString id;              // ANTS-NNNN this block shipped as
    QString session;         // stub breadcrumb author (caller-resolved default)
    QString date;            // stub breadcrumb date  (caller-resolved default)
};

struct CompactOutcome {
    QString heading, id;
    bool    applied = false;
    QString code, reason;               // code == "" on success
    int     startLine = -1, endLine = -1;   // 1-based block range (heading..last body line)
    int     bytesBefore = 0, bytesAfter = 0;
    QVector<int> candidates;            // target_ambiguous only: colliding heading lines
};

struct CompactResult {
    QString newContent;                 // full file after all applied collapses
    QVector<CompactOutcome> results;    // one per input target, input order
    long    bytesSaved = 0;             // signed Σ(bytesBefore − bytesAfter) over applied
};

// Pure. Runs parse(content) internally for the watermark + tracking rows,
// resolves + gates each target (§2.3), and collapses applied blocks
// bottom-up in one pass. Targets are assumed request-shape-valid (non-empty
// heading, id matching ANTS-NNNN) — the wrapper enforces that (bad_args).
CompactResult compactShipped(const QString &content,
                             const QVector<CompactTarget> &targets);

// ---- ANTS-3442: maintainer row-dedup (prune_tracking) ---------------
//
// Remove superseded duplicate maintainer tracking-table rows, keeping the
// authoritative last-per-id row. Two-stage pass (docs/specs/ANTS-3442.md
// § 2.3): Stage 1 marks id-column-superseded rows; Stage 2 removes a marked
// row only when every ANTS-NNNN token anywhere in its line still appears in
// a surviving line, so parse().mappedIds is preserved.

struct PruneOptions {
    QStringList scopeIds;      // restrict to these ANTS-NNNN ids; empty ⟹ all
};

struct PrunedRow {
    QStringList ids;           // the removed row's id column
    QString     status;
    int         line = -1;     // original 1-based line
};

struct PruneResult {
    QString            newContent;   // full file after removals
    QVector<PrunedRow> removed;      // in original document order
    long               bytesSaved = 0;
};

// Pure. Runs parse(content) internally, applies the two-stage selection, and
// removes the chosen rows bottom-up in one pass. scopeIds are assumed
// request-shape-valid (each matches ANTS-NNNN) — the wrapper enforces that
// (bad_args) and the absent-file / path plumbing.
PruneResult pruneTracking(const QString &content, const PruneOptions &opts);

// ---- ANTS-3443: maintainer v2 compaction (compact_resolved) ---------
//
// Collapse a shipped v2 finding's write-up to a roadmap-driven stub,
// keeping the `### ` heading AND the finding's first `**Proposed ID:**`
// line verbatim. The roadmap truth is injected pre-resolved (two id sets)
// so the helper stays MainWindow-free and unit-testable over synthetic
// content — the same posture as pruneTracking. See docs/specs/ANTS-3443.md
// for the full contract (gates + invariants).

// A `### ` finding sub-block located by the fence-aware enumerator
// (enumerateFindingBlocks). Shared with the pending v2 feedback_query
// delta parser + op:migrate_v2 (spec § 2.6 — one scanner, so consumers
// never drift on block extents or on which line is the authoritative id
// line). idLine0 < 0 ⟹ the block carries no `**Proposed ID:**` line: it is
// non-finding prose, not a finding. Each consumer layers its own
// classification (feedback_query flags a line-less finding-shaped block as
// suspected_untagged; compact_resolved treats it as inert prose).
struct FindingBlock {
    int     headingLine0 = -1;  // 0-based index of the `### ` heading line
    QString heading;            // the heading line, verbatim
    int     extentEnd0 = -1;    // 0-based exclusive end: next `#`/`## `/`### `
                                // boundary (or EOF), fences skipped
    int     idLine0 = -1;       // 0-based index of the FIRST `**Proposed ID:**`
                                // line in the body, or -1 when absent
    QString idValue;            // that line's capture-group-1 value, trimmed of
                                // whitespace + stray `*`; "" when idLine0 < 0
};

// Enumerate every `### ` finding sub-block outside fenced regions, in
// document order. Uses the standard's canonical id-line regex
// (mcp-feedback-files.md § "Maintainer triage") to find each block's first
// `**Proposed ID:**` line; the classification (finding vs prose) is left to
// the caller (idLine0 >= 0 ⟹ the block has an id line).
QVector<FindingBlock> enumerateFindingBlocks(const QStringList &lines);

struct ResolveOptions {
    QSet<QString> shippedIds;   // canonical ids whose live roadmap status is ✅
    QSet<QString> roadmapIds;   // every canonical id present in the roadmap
                                // (any status) — an id absent from this set is
                                // "unresolved" (archive-rotated / unknown)
};

struct ResolvedFinding {
    QString     heading;
    QStringList ids;            // ANTS-NNNN tokens from the first Proposed-ID line
    bool        collapsed = false;
    QString     code;           // "" when collapsed; else the first-failing skip
                                // code (no_shippable_id / already_compacted /
                                // roadmap_unresolved_ids / has_open_id)
    QStringList openIds;        // has_open_id: the present-but-not-✅ subset
    QStringList unresolvedIds;  // roadmap_unresolved_ids: the not-in-roadmap subset
    int         line = -1;      // 1-based heading line
    int         bytesBefore = 0, bytesAfter = 0;  // populated on collapse
};

struct ResolveResult {
    QString newContent;                    // full file after all collapses
    QVector<ResolvedFinding> findings;     // real findings only, in document
                                           // order; id-less prose blocks omitted
    long    bytesSaved = 0;                // signed Σ(bytesBefore − bytesAfter)
};

// Pure. Enumerate findings, gate each on the injected roadmap sets
// (spec § 2.5, first-failure-wins), and collapse the all-✅ findings
// bottom-up in one pass. shippedIds/roadmapIds are assumed pre-resolved by
// the wrapper (RoadmapDialog::parseBullets); the wrapper also owns the
// version gate, the roadmap read, and the atomic write.
ResolveResult compactResolved(const QString &content, const ResolveOptions &opts);

// ---- ANTS-3446: one-shot v1→v2 migration (migrate_v2) ---------------
//
// Mechanical, leave-tables-in-place v1→v2 converter: bump the version
// marker to `: 2` and stamp a blank `**Proposed ID:** _(maintainer to
// assign)_` line on every finding-shaped, below-watermark `### ` block
// that lacks one. The v1 tracking tables are NOT moved / collapsed /
// deleted, so the v1 watermark — and thus the shipped un-triaged delta —
// is preserved (spec § 1.1 / INV-4). Pure; no roadmap, no filesystem —
// same posture as compactResolved / pruneTracking. See
// docs/specs/ANTS-3446.md for the full contract.

struct MigrateStamp {
    QString heading;      // the `### ` heading line, verbatim
    int     line = -1;    // 1-based heading line in the ORIGINAL file
};

struct MigrateOrphan {
    QString heading;
    int     line = -1;    // 1-based heading line in the ORIGINAL file
    QString reason;       // "finding_shaped_above_watermark" (the only class)
};

struct MigrateResult {
    QString newContent;                  // file after marker bump + stamps
    bool    alreadyV2 = false;           // marker ≥ 2 ⟹ clean byte-identical no-op
    QVector<MigrateStamp>  stamped;      // findings given a blank Proposed-ID line
    QVector<MigrateOrphan> orphans;      // finding-shaped, above the watermark
    QVector<MigrateStamp>  unclassified; // below-watermark `### ` block, not finding-shaped
    long    bytesDelta = 0;              // signed Σ (newContent − content) sizes
};

// Pure. Runs parse() for the watermark (lastMaintainerLine) and
// enumerateFindingBlocks() for the `### ` blocks, classifies each line-less
// block by (finding-shaped × position) per spec § 2.4, stamps the
// below-watermark findings bottom-up, then bumps the marker. A marker ≥ 2
// short-circuits to a byte-identical no-op (alreadyV2). No table is moved
// (INV-4). The wrapper (cmdFeedbackLog) owns path resolution, the suffix
// guard, dry_run, and the atomic write — there is no roadmap read.
MigrateResult migrateV2(const QString &content);

}  // namespace FeedbackFile
