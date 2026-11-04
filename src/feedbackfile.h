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

// ANTS-4702 — the highest format version this build defines. The delta rule
// is "v2 or higher", so a file marked with anything above this is treated as
// v2 and everything works — which is exactly why a wrong marker stays
// invisible until some verb gates on an exact version, and then fails far
// from the file that caused it. Callers are told instead of widened silently.
constexpr int kMaxKnownFormatVersion = 2;


// ---- ANTS-1961: read side (the un-triaged delta parser) -------------

// A maintainer tracking-table row from a v1-era file, surfaced on the read
// side (ANTS-3371, ParseResult::trackingRows). Defined before ParseResult so
// the read parser can carry the structured rows.
struct TrackingRow {
    QString     item;
    QStringList ids;     // ANTS-NNNN strings; empty → renders `n/a`
    QString     status;  // an emoji from the machine-readable set
    QString     notes;   // optional; triggers the 4th column when any row has it
    int         line = -1;  // ANTS-3442: 1-based source line of this data row
};

// ANTS-3448 — a `### ` finding-shaped block a hand editor left with NO
// `**Proposed ID:**` line (a silent-loss risk). Recorded only under the v2
// rule so feedback_query can surface it rather than let it vanish as prose.
struct SuspectedFinding {
    QString heading;      // the `### ` heading line, verbatim
    int     line = -1;    // 1-based heading line
};

// ANTS-3631 — a finding the maintainer answered with a QUESTION rather than an
// id or a closure. Un-triaged on purpose, so it is BOTH in `delta` (where the
// reporter reads it) and here (where the maintainer sees which of their own
// questions are still outstanding). Two un-triaged values are not the same
// thing: a placeholder means nobody has looked, a marker means the maintainer
// looked and is waiting — and a caller that treats them alike reports the
// maintainer's outbox as their inbox.
struct AwaitingFinding {
    QString heading;
    int     line = -1;
    QString question;   // may be empty; the marker classifies without it
};

// ANTS-4513 § 4.1 — one `### ` finding as parse() classified it. Filled by the
// same tests that build `delta`, `awaiting` and `suspectedUntagged`, so the
// store's finding rows are a copy of parse()'s verdict, never a second one.
// An id-less block that is not finding-shaped is prose and gets no record.
struct FindingRecord {
    QString     heading;        // the `### ` heading line, verbatim
    int         line = -1;      // 1-based heading line
    QString     entryHeading;   // the enclosing `#`/`## ` heading, verbatim; "" when none
    QString     filedOn;        // entryHeading's leading YYYY-MM-DD; "" when none
    QString     triage;         // untriaged | awaiting | closed | mapped | untagged
    QStringList ids;            // mapped only: the ids, in Proposed ID order
};

struct ParseResult {
    QString     delta;              // v1: text from the first contributor
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
                                     // ANTS-3744: when no finding yields an
                                     // id, the ids on `Tracked in ROADMAP`
                                     // headings instead.
    int         formatVersion = 0;   // ANTS-3448: version from the first
                                     // `<!-- ants-mcp-feedback: N -->` marker
                                     // (0 when absent/malformed). >= 2 ⟹ the v2
                                     // delta rule was applied.
    // ANTS-3631 — the awaiting subset, in document order. A SUBSET of what
    // `delta` carries, never a partition of it: an awaiting finding is in both,
    // because the reporter must see the question and the maintainer must see
    // that it is unanswered.
    QVector<AwaitingFinding> awaiting;
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
    QVector<FindingRecord> findings;     // ANTS-4513: v2-only, document order;
                                         // always empty under v1.
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
// marker (digit-optional); 0 when the marker is absent or carries no digits.
// parse() and the store's import gate (ANTS-4513, not_v2) both read it.
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

// Render the file skeleton for an absent file: the marker comment + a
// derived `# <projectTitle> …` H1 + the contributor-pointer blockquote.
QString skeleton(const QString &projectTitle);

// ---- ANTS-3443: the `### ` finding scanner ---------------------------

// A `### ` finding sub-block located by the fence-aware enumerator
// (enumerateFindingBlocks). Shared by the v2 delta parser and assign_id, so
// they never drift on block extents or on which line is the authoritative id
// line. idLine0 < 0 ⟹ the block carries no `**Proposed ID:**` line: it is
// non-finding prose, not a finding. parse() flags a line-less finding-shaped
// block as suspected_untagged.
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

// ANTS-3504 — extract a roadmap bullet's ship-date from its body: the ISO date
// captured by the LAST body line matching `^\s*Resolved\s+\(?(\d{4}-\d{2}-\d{2})`
// (a line-leading `Resolved` marker, date with or without parentheses — the
// corpus uses both). "" when no line matches (mid-line / "Resolved as of …" /
// no `Resolved` line → graceful dateless fallback). feedback_query's
// shipped_date reads it.
// See docs/specs/ANTS-3504.md § 2.1.
QString shipDateFromRoadmapBody(const QString &roadmapBulletBody);

// ---- ANTS-4646(b): set the file's H1 (set_title) ---------------------
//
// The FILENAME is derived from caller_cwd's leaf and is always right; the H1
// was writable by no op, so a project rename left the title contradicting the
// filename and the only route was a hand edit — against the file's own
// "don't hand-edit" instruction, at the moment a session is most likely to
// get it wrong.

struct SetTitleResult {
    QString newContent;
    QString oldTitle;
    QString newTitle;
    bool    changed = false;
    QString code;        // "" on success; "no_h1" when the file has no H1
};

// Rewrite the first non-fenced `# ` line's project name. The existing prefix
// is PRESERVED verbatim (the corpus carries both "Ants MCP feedback" and
// "Ants MCP Feedback"): only the text after the last em dash is replaced, and
// a prefix-less H1 is rewritten whole. Never invents an H1 — that would guess
// at a position the format fixes.
SetTitleResult setTitle(const QString &content, const QString &projectTitle);

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
    QString note;             // ANTS-3571 — optional single-line closure note;
                              // written as a `- **Note:**` bullet under the id
                              // line (wrapper folds newlines to spaces). Empty
                              // ⟹ no note bullet touched.
};

struct AssignResult {
    QString newContent;       // full file after the replace/insert
    QString heading;          // the resolved `### ` heading, verbatim
    int     line = -1;        // 1-based `### ` heading line in the ORIGINAL file
    QString value;            // the id-line value written (echoes target.value)
    bool    inserted = false; // true ⟹ the finding had no id line (inserted)
    bool    changed = false;  // false ⟹ file byte-identical (id AND note no-op)
    bool    noteWritten = false; // ANTS-3571 — a note bullet was rendered
    long    bytesDelta = 0;   // signed size change
    QString code;             // "" on success; else target_not_found /
                              // target_ambiguous
    QVector<int> candidates;  // target_ambiguous only: colliding 1-based `### ` lines
    QStringList headings;     // target_not_found only: every `### ` heading, in file order
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
