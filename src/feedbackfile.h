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

#include <QHash>
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

// ANTS-3448 — a `### ` finding-shaped block a hand editor left with NO
// `**Proposed ID:**` line (a silent-loss risk). Recorded only under the v2
// rule so feedback_query can surface it rather than let it vanish as prose.
struct SuspectedFinding {
    QString heading;      // the `### ` heading line, verbatim
    int     line = -1;    // 1-based heading line
};

struct ParseResult {
    QString     delta;               // v1: text from the first contributor
                                     // heading after the last maintainer
                                     // heading to EOF. v2 (ANTS-3448): the
                                     // ordered concatenation of every un-triaged
                                     // finding's block, `\n`-joined — NOT a
                                     // contiguous file slice (INV-11).
    bool        deltaPresent = false;
    int         deltaStartLine = -1; // 1-based line of the FIRST delta line.
                                     // v1: the delta's first line. v2: the first
                                     // un-triaged finding's heading line —
                                     // informational, NOT a slice anchor (the
                                     // v2 delta is a concatenation; do not
                                     // re-derive `delta` from it — INV-11). -1
                                     // when empty.
    int         deltaLineCount = 0;  // lines in the full (pre-cap) delta text.
                                     // v1: a contiguous slice count; v2: the
                                     // summed line count of the concatenated
                                     // un-triaged findings (INV-11).
    QStringList mappedIds;           // unique, sorted. v1: ANTS-[0-9]+ from
                                     // maintainer-block bodies. v2 (ANTS-3448):
                                     // the union of ANTS-[0-9]+ from each
                                     // finding's first `**Proposed ID:**` value
                                     // (n/a closures contribute none); the
                                     // retained v1 tables are not counted.
    int         formatVersion = 0;   // ANTS-3448: version from the first
                                     // `<!-- ants-mcp-feedback: N -->` marker
                                     // (0 when absent/malformed). >= 2 ⟹ the v2
                                     // delta rule was applied.
    QVector<SuspectedFinding> suspectedUntagged;  // ANTS-3448: v2-only; `### `
                                     // finding-shaped blocks with no id line.
                                     // Always empty under v1.
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
// maintainer headings identified by the anchor regex. ANTS-3448:
// marker-aware — a `<!-- ants-mcp-feedback: 2 -->` (or higher) file's delta /
// mappedIds / suspectedUntagged follow the v2 inline-`**Proposed ID:**` rule;
// the v1 boundary scan (lastMaintainerLine / maintainerBlockCount /
// trackingRows) runs unchanged on both versions.
ParseResult parse(const QString &fileContent);

// ANTS-3448 — version int from the first `<!-- ants-mcp-feedback: N -->`
// marker (the digit-optional regex migrate_v2 uses); 0 when the marker is
// absent or carries no digits. Shared by parse() (read side) and migrateV2()
// (write side) so both extract the version identically (INV-12). Returns the
// version only — migrateV2 keeps its own marker presence + line-index scan for
// its insert-vs-repair branch.
int markerVersion(const QString &content);

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
// (enumerateFindingBlocks). Shared with the v2 feedback_query
// delta parser + op:migrate_v2 (both shipped; spec § 2.6 — one scanner, so consumers
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
    // ANTS-3504 — id → ship-date (ISO "YYYY-MM-DD"): the date on the id's LAST
    // roadmap `Resolved` line (parens optional; shipDateFromRoadmapBody). Filled
    // by the cmdFeedbackLog wrapper from the same parseBullets pass. An id absent
    // here has no ship-date; compactResolved stamps the max across a finding's
    // ids into the stub, or the dateless breadcrumb when none maps.
    QHash<QString, QString> shipDates;
};

// ANTS-3504 — extract a roadmap bullet's ship-date from its body: the ISO date
// captured by the LAST body line matching `^\s*Resolved\s+\(?(\d{4}-\d{2}-\d{2})`
// (a line-leading `Resolved` marker, date with or without parentheses — the
// corpus uses both). "" when no line matches (mid-line / "Resolved as of …" /
// no `Resolved` line → graceful dateless fallback). The single extractor both
// feedback surfaces share (compact_resolved stub + feedback_query shipped_date).
// See docs/specs/ANTS-3504.md § 2.1.
QString shipDateFromRoadmapBody(const QString &roadmapBulletBody);

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

// ANTS-3474 — a finding whose inline Proposed-ID was backfilled from the v1
// tracking tables during migrate_v2 (instead of stamped blank).
struct MigrateBackfill {
    QString heading;         // the `### ` heading line, verbatim
    int     line = -1;       // 1-based heading line in the ORIGINAL file
    QString id;              // the ANTS-NNNN(s) stamped inline (row's ids joined)
    int     confidencePct = 0;  // heading↔item overlap-coefficient, 0..100
};

struct MigrateResult {
    QString newContent;                  // file after marker bump + stamps
    bool    alreadyV2 = false;           // marker ≥ 2 ⟹ clean byte-identical no-op
    QVector<MigrateStamp>  stamped;      // findings given a blank Proposed-ID line
    QVector<MigrateBackfill> backfilled; // ANTS-3474 — findings given a tracking-table id
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
//
// ANTS-3474 — when `backfillFromTracking` is true, each finding that would be
// stamped blank is first token-matched (heading ↔ tracking-row `item`) against
// the file's own v1 tracking rows; a single id clearing a high confidence
// threshold with a clear margin is stamped INLINE (recorded in `backfilled`)
// instead of blank. Ambiguous / low-confidence findings stay blank (never a
// wrong id) — precision over recall, misses fall to manual assign_id.
MigrateResult migrateV2(const QString &content,
                        bool backfillFromTracking = false);

// ---- ANTS-3447: v2 inline triage write (assign_id) ------------------
//
// Fill one `### ` finding's `**Proposed ID:**` line in place: replace an
// existing / placeholder line, or insert one as the first body bullet when
// absent. Records the maintainer's assigned id(s) or an `n/a — <reason>`
// closure. Pure; no roadmap, no filesystem — the wrapper (cmdFeedbackLog)
// owns path resolution, the suffix guard, dry_run, and the atomic write.
// See docs/specs/ANTS-3447.md for the full contract.

struct AssignTarget {
    QString heading;          // the target `### ` heading, verbatim (trimmed match)
    int     headingLine = -1; // optional 1-based `### ` line disambiguator
    QString value;            // the composed id-line value the wrapper built
                              // ("ANTS-1525, ANTS-1526" or "n/a — <reason>" / "n/a")
    bool    isClosure = false;// wrapper sets true when `value` is an n/a closure
                              // (informational; not load-bearing in the helper)
};

struct AssignResult {
    QString newContent;       // full file after the replace/insert
    QString heading;          // the resolved `### ` heading, verbatim
    int     line = -1;        // 1-based `### ` heading line in the ORIGINAL file
    QString value;            // the id-line value written (echoes target.value)
    bool    inserted = false; // true ⟹ the finding had no id line (inserted)
    bool    changed = false;  // false ⟹ the line already held `value` (no-op)
    long    bytesDelta = 0;   // signed size change
    QString code;             // "" on success; else target_not_found /
                              // target_ambiguous
    QVector<int> candidates;  // target_ambiguous only: colliding 1-based `### ` lines
};

// Pure. Enumerate `### ` findings (enumerateFindingBlocks), resolve
// target.heading (+ optional headingLine) to exactly one block
// (target_not_found / target_ambiguous), then replace its first
// `**Proposed ID:**` line with `- **Proposed ID:** <value>` or insert that
// line as the first body bullet when absent. Request-shape validity
// (hasIds XOR hasClosure, ^ANTS-[0-9]+$ ids, newline-folded closure) is the
// wrapper's job — the helper trusts target.value.
AssignResult assignId(const QString &content, const AssignTarget &target);

}  // namespace FeedbackFile
