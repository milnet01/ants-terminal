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

}  // namespace FeedbackFile
