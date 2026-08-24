// ANTS-1548: pure helpers for the changelog_log MCP tool — render a
// Keep-a-Changelog entry and splice it under `## [Unreleased]` in a
// CHANGELOG.md body. Qt6::Core-only; lives in ants_core_lib so the
// remotecontrol handler and the feature test share one implementation.
// Contract: tests/features/changelog_log_writer/spec.md (no docs/specs
// file was written for ANTS-1548).

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace ChangelogLog {

// The six canonical Keep-a-Changelog categories, in spec order.
// kindToCategory maps a roadmap `Kind:` value onto one of these so a
// caller can cite a bullet by id without restating the category.
QString kindToCategory(const QString &kind);

// True iff `category` is one of the six canonical categories.
bool isValidCategory(const QString &category);

// The six canonical Keep-a-Changelog categories, in spec order
// (Added/Changed/Deprecated/Removed/Fixed/Security). Exposed publicly
// (ANTS-3533) so changelog_query can emit the `accepted[]` list in a
// `bad_category` refusal; previously a file-private helper.
const QStringList &canonicalCategories();

// Render a single Keep-a-Changelog bullet block:
//   - **<summary>** (<id>)
//     <body line 1>
//     <body line 2>
// `summary` is bold; the trailing `(<id>)` is appended only when `id`
// is non-empty. `body` (optional, may be multi-line) is indented two
// spaces per line; blank source lines stay blank. No trailing newline.
QString formatBullet(const QString &summary, const QString &body,
                     const QString &id);

// ANTS-4629 — why a caller-supplied `summary` must be refused, or empty when it
// is fine. formatBullet() wraps in `**` and appends the id unconditionally, so
// a summary already carrying either renders as an empty-strong followed by
// literal asterisks: a bullet that does not read as a heading, written with
// ok:true. Only for text the CALLER supplied — a summary this verb derives
// from a roadmap headline is its own and is never checked.
QString preRenderedSummaryReason(const QString &summary, const QString &id);

struct InsertResult {
    bool    ok = false;
    QString markdown;   // the new file body (valid iff ok)
    QString code;       // refusal code iff !ok
    QString error;      // human-readable message iff !ok
    int     line = -1;  // 1-based line the bullet was inserted at (iff ok)
    bool    created_category = false;  // a new ### heading was added
    // ANTS-2125 — non-blocking advisory: the Unreleased section already
    // interleaves non-heading prose (a stray footer/separator/paragraph)
    // between its `### <category>` blocks, so the insert — though placed
    // in canonical order — lands in a malformed section. `malformed_line`
    // is the 1-based line of the first offending prose line (iff
    // malformed_section). Detected on the pre-insert body; insertion
    // behaviour is unaffected.
    bool    malformed_section = false;
    int     malformed_line = -1;
};

// Insert `bulletBlock` (from formatBullet) at the TOP of `category`'s
// list within the `## [Unreleased]` section of `markdown`. Creates the
// `### <category>` heading in canonical order if it is absent. Refusals:
//   not_unreleased  — no `## [Unreleased]` heading
//   bad_category    — category not one of the six canonical values
InsertResult insertUnreleasedEntry(const QString &markdown,
                                   const QString &category,
                                   const QString &bulletBlock);

struct SubsectionResult {
    bool    ok = false;
    QString markdown;   // new body (valid iff ok)
    QString code;       // refusal code iff !ok
    QString error;      // human message iff !ok
    int     line = -1;  // 1-based line of the inserted `### ` heading (iff ok)
};

// ANTS-3584 — insert a DATED feature-grouped subsection at the TOP of the
// `## [Unreleased]` section (newest-first), for CHANGELOGs that group by dated
// topic (`### <date> <Category> — <headline>` + prose + bullets) rather than
// flat Keep-a-Changelog categories (the Vestige/3D_Engine house style). Opt-in:
// the flat-category `insertUnreleasedEntry` path is unchanged. Emits:
//   ### <date> <Category> — <headline>
//   <blank>
//   <body prose, flush-left, verbatim lines>   (omitted when body is blank)
//   <blank>
//   <bulletBlocks, one blank line between>      (omitted when none)
//   <blank>
// `bulletBlocks` are pre-rendered via formatBullet(). A missing blank spacer
// after `## [Unreleased]` is repaired so the heading never abuts the block.
// Refusals:
//   not_unreleased  — no `## [Unreleased]` heading
//   bad_category    — category not one of the six canonical values
// (date/headline emptiness is a verb-layer `missing_field` guard.)
SubsectionResult insertUnreleasedSubsection(const QString &markdown,
                                            const QString &date,
                                            const QString &category,
                                            const QString &headline,
                                            const QString &body,
                                            const QStringList &bulletBlocks);

// ANTS-3495 — op:normalize. Reorder the `### <category>` blocks inside
// `## [Unreleased]` into canonical Keep-a-Changelog order (Added/Changed/
// Deprecated/Removed/Fixed/Security). Non-destructive: each block's body
// (its bullets and any interleaved prose) moves with its heading. A
// duplicate or non-canonical `### ` heading keeps its relative position
// (stable sort; unknown categories sort last). Content before the first
// `### ` heading (a preamble paragraph) is preserved untouched.
//
// ANTS-3381 — normalize also RELOCATES stray interleaved prose, the half
// ANTS-3495 deferred pending a decided policy. Policy (accepted by the
// user 2026-08-14): a flagged prose line is folded into the nearest
// preceding bullet as a two-space continuation. Because such a line
// always sits after that bullet with only blanks, continuations and HTML
// comments between them, the fold is an in-place re-indent — the line
// count never changes, so no bullet's content moves and no ordering is
// disturbed. Each fold is reported in `prose_moves` so `dry_run` can
// preview it; the accepted failure mode is a paragraph that was meant to
// stand alone being absorbed by the entry above it, and the preview is
// the only thing that catches that. Prose separated from its bullet by a
// heading of any depth is NOT folded (the heading is a barrier, so the
// fold would not put the line under the bullet at all) and keeps
// tripping the `malformed_section` advisory.
struct ProseMove {
    int     from_line = -1;   // 1-based line of the prose, before the fold
    int     under_line = -1;  // 1-based line of the bullet it folds into
    QString text;             // the folded line, trimmed
};

// ANTS-4363 — CLOSE `## [Unreleased]` into a version block: rename it to
// `## [X.Y.Z] - <date>` and open a fresh empty `## [Unreleased]` above. This
// is the one changelog edit every release makes, and the only one the verb
// did not own — so a file the verb otherwise writes had its version headings
// typed by hand, at the highest-stakes moment. A project whose release
// workflow greps `^## \[<version>\]` to extract release notes publishes an
// EMPTY body when that heading is a character out.
//
// `released_body` is the closed section's content, because a caller wants it
// as release notes immediately after.
struct ReleaseResult {
    bool    ok = false;
    QString markdown;        // new body (valid iff ok)
    QString code;            // refusal code iff !ok
    QString error;           // human message iff !ok
    QString heading;         // the heading written (iff ok)
    QString released_body;   // the closed section's content (iff ok)
    int     line = -1;       // 1-based line of the new version heading
};

// Refuses `nothing_to_release` on an empty `[Unreleased]` (a version block
// with no entries is worse than no release) and `version_exists` when the
// version already has a heading — re-cutting one silently would produce two
// blocks the notes-extraction grep cannot choose between. `date` empty ⟹
// today, matching the sibling ops.
ReleaseResult closeUnreleased(const QString &markdown,
                              const QString &version,
                              const QString &date = QString());

struct NormalizeResult {
    bool        ok = false;
    QString     markdown;             // new body (valid iff ok)
    QString     code;                 // refusal code iff !ok
    QString     error;                // human message iff !ok
    bool        changed = false;      // a block moved, or prose was folded
    QStringList order_before;         // category headings, original order
    QStringList order_after;          // category headings, canonical order
    // ANTS-3381 — one entry per folded prose line, in file order. Empty
    // when the section carried no relocatable prose. Line numbers are
    // against the INPUT body (the fold preserves the line count, so they
    // are also valid against the result).
    QVector<ProseMove> prose_moves;
    // Non-blocking advisory (parity with insertUnreleasedEntry): after
    // the reorder the section still interleaves non-heading prose
    // between `### ` blocks. `malformed_line` is the 1-based line of the
    // first offending line in the RESULT body (iff malformed_section).
    // Since ANTS-3381 this can only be prose behind a heading barrier —
    // everything foldable has already been folded.
    bool        malformed_section = false;
    int         malformed_line = -1;
};

// Canonicalise the ordering of the `### <category>` blocks under
// `## [Unreleased]` in `markdown`, and fold stray interleaved prose into
// the bullet above it (ANTS-3381). Refusals:
//   not_unreleased          — no `## [Unreleased]` heading
//   feature_grouped_section — section uses dated `### ` topic
//                             subsections, not flat categories (parity
//                             with insertUnreleasedEntry; reordering
//                             dated topics by category is meaningless)
NormalizeResult normalizeUnreleased(const QString &markdown);

}  // namespace ChangelogLog
