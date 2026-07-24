// ANTS-1548: pure helpers for the changelog_log MCP tool — render a
// Keep-a-Changelog entry and splice it under `## [Unreleased]` in a
// CHANGELOG.md body. Qt6::Core-only; lives in ants_core_lib so the
// remotecontrol handler and the feature test share one implementation.
// See docs/specs/ANTS-1548.md.

#pragma once

#include <QString>
#include <QStringList>

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

// ANTS-3495 — op:normalize (reorder-only subset). Reorder the
// `### <category>` blocks inside `## [Unreleased]` into canonical
// Keep-a-Changelog order (Added/Changed/Deprecated/Removed/Fixed/
// Security). Non-destructive: each block's body (its bullets and any
// interleaved prose) moves with its heading — relocating stray prose
// out of the wrong block is a deliberately deferred follow-up
// (ROADMAP notes the policy is still undecided). A duplicate or
// non-canonical `### ` heading keeps its relative position (stable
// sort; unknown categories sort last). Content before the first
// `### ` heading (a preamble paragraph) is preserved untouched.
struct NormalizeResult {
    bool        ok = false;
    QString     markdown;             // new body (valid iff ok)
    QString     code;                 // refusal code iff !ok
    QString     error;                // human message iff !ok
    bool        changed = false;      // true iff the reorder moved a block
    QStringList order_before;         // category headings, original order
    QStringList order_after;          // category headings, canonical order
    // Non-blocking advisory (parity with insertUnreleasedEntry): after
    // the reorder the section still interleaves non-heading prose
    // between `### ` blocks. `malformed_line` is the 1-based line of the
    // first offending line in the RESULT body (iff malformed_section).
    bool        malformed_section = false;
    int         malformed_line = -1;
};

// Canonicalise the ordering of the `### <category>` blocks under
// `## [Unreleased]` in `markdown`. Refusals:
//   not_unreleased          — no `## [Unreleased]` heading
//   feature_grouped_section — section uses dated `### ` topic
//                             subsections, not flat categories (parity
//                             with insertUnreleasedEntry; reordering
//                             dated topics by category is meaningless)
NormalizeResult normalizeUnreleased(const QString &markdown);

}  // namespace ChangelogLog
