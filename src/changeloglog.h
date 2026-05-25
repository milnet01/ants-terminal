// ANTS-1548: pure helpers for the changelog_log MCP tool — render a
// Keep-a-Changelog entry and splice it under `## [Unreleased]` in a
// CHANGELOG.md body. Qt6::Core-only; lives in ants_core_lib so the
// remotecontrol handler and the feature test share one implementation.
// See docs/specs/ANTS-1548.md.

#pragma once

#include <QString>

namespace ChangelogLog {

// The six canonical Keep-a-Changelog categories, in spec order.
// kindToCategory maps a roadmap `Kind:` value onto one of these so a
// caller can cite a bullet by id without restating the category.
QString kindToCategory(const QString &kind);

// True iff `category` is one of the six canonical categories.
bool isValidCategory(const QString &category);

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
};

// Insert `bulletBlock` (from formatBullet) at the TOP of `category`'s
// list within the `## [Unreleased]` section of `markdown`. Creates the
// `### <category>` heading in canonical order if it is absent. Refusals:
//   not_unreleased  — no `## [Unreleased]` heading
//   bad_category    — category not one of the six canonical values
InsertResult insertUnreleasedEntry(const QString &markdown,
                                   const QString &category,
                                   const QString &bulletBlock);

}  // namespace ChangelogLog
