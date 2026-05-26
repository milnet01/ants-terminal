# ANTS-1548 — changelog_log MCP tool

## Problem

The per-change CHANGELOG entry is a hand-written Edit (read the file for
context, then format a Keep-a-Changelog bullet under the right
`### <category>` in `## [Unreleased]`). That costs formatting prose +
a Read every time. `changelog_log` renders + splices the entry
server-side.

This ships the two token-saving modes against the project's actual
Keep-a-Changelog format (the 2026-05-18 proposal's "emoji-dated
section" shape does not match the current CHANGELOG, which uses standard
Added/Changed/Deprecated/Removed/Fixed/Security categories). The
`roadmap_log op:flip also_write_changelog` integration (mode 3) is
deferred — tracked separately.

## Invariants

### INV-1 — kind → category mapping

`ChangelogLog::kindToCategory` maps roadmap `Kind:` values onto the six
canonical categories: feature/implement/enhancement → Added;
fix/doc-fix/audit-fix/review-fix → Fixed; security → Security; all
other kinds → Changed.

### INV-2 — bullet formatting

`ChangelogLog::formatBullet(summary, body, id)` renders
`- **<summary>**`, appends ` (<id>)` only when `id` is non-empty, and
indents each non-empty `body` line two spaces. No trailing newline.

### INV-3 — insert under existing category, most-recent-first

`insertUnreleasedEntry` inserts the bullet at the TOP of the named
category's list within `## [Unreleased]` (right after the heading +
spacer), leaving older entries below.

### INV-4 — create missing category in canonical order

When the category heading is absent, it is created in canonical order
(Added, Changed, Deprecated, Removed, Fixed, Security) within the
Unreleased section, and `created_category` is reported.

### INV-5 — refusals

`not_unreleased` when there is no `## [Unreleased]` heading;
`bad_category` for a non-canonical category. The `changelog_log`
handler adds `no_changelog`, `id_not_in_roadmap` (add_from_roadmap),
`missing_field`, and `bad_op_combo`.

### INV-6 — op:"add" end to end

`cmdChangelogLog` op:"add" with {caller_cwd, summary, kind} writes the
bullet under the derived category in the project's CHANGELOG.md
atomically, returning {ok, op, file, category, line, bytes_written}.

### INV-7 — op:"add_from_roadmap" reuses bullet prose

op:"add_from_roadmap" with {caller_cwd, id} pulls the cited ROADMAP
bullet's headline (→ summary) and `Layman:` line (→ body) verbatim and
derives the category from the bullet's `Kind:`. A missing id refuses
`id_not_in_roadmap`.

The reused headline is collapsed to a single line via the same
newline/whitespace-run folding `roadmap_query` already applies to
`headline_oneline` (ANTS-1868) — a wrapped multi-line ROADMAP
headline cannot leak a hard newline into the rendered CHANGELOG
bullet's bold summary.

### INV-8 — caller_cwd Required + descriptor

`changelog_log` is classified `Required` in `callerCwdContractFor` and
registered with the Required contract; the descriptor names the tool
and its ops.

## Test plan

INV-1..5 are exercised behaviourally on the pure `ChangelogLog` helpers.
INV-6/7 drive `cmdChangelogLog` against a seeded temp project
(CHANGELOG.md + ROADMAP.md). INV-8 is source-scrape.
