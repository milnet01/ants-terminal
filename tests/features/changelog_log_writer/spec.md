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
`bad_category` for a non-canonical category;
`feature_grouped_section` (INV-11) when the section is feature-grouped.
The `changelog_log` handler adds `no_changelog`, `id_not_in_roadmap`
(add_from_roadmap), `missing_field`, and `bad_op_combo`.

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

### INV-9 — malformed-section advisory (ANTS-2125)

When the `## [Unreleased]` section already interleaves non-heading prose
(a `---` rule, a stray footer/separator, a flush-left paragraph) between
its `### <category>` blocks, the insert still lands in canonical order
but the result carries a non-blocking advisory:

- `insertUnreleasedEntry` sets `malformed_section = true` and
  `malformed_line` to the 1-based first offending line (detected on the
  pre-insert body); a clean section leaves `malformed_section = false`
  and `malformed_line = -1`.
- `cmdChangelogLog` / the `add_batch` envelope carries an `advisory`
  string on a successful write into a malformed section, omitted for a
  clean one.

Scanning starts only once the first `### ` category heading is seen, so
a legitimate description paragraph directly under `## [Unreleased]`
(before any category) is not flagged. Mirrors roadmap_log's
`possible_duplicates` advisory shape.

### INV-10 — add_from_roadmap reuses the untruncated headline (ANTS-2127)

`op:"add_from_roadmap"` builds the bold summary from the bullet's
untruncated headline (`BulletRecord.headlineFull`, ANTS-2075), not the
120-char display cap (`headline`, ANTS-1811). A roadmap headline longer
than 120 chars renders in full in the CHANGELOG with no `…` ellipsis
leaked into the bold span. Applies to both the single op and the
`add_batch` per-entry path.

### INV-11 — feature-grouped section refusal (ANTS-3416)

When the `## [Unreleased]` section is **feature-grouped** — its direct
`### ` children are dated topic headings (`### <id> — <topic> (<date>)`,
newest-first, the MAME Curator house style) with `**Bold**` category runs
(`**Fixed**`, inline `**Security:**`) beneath, rather than flat
Keep-a-Changelog `### <category>` blocks — `insertUnreleasedEntry` refuses
with `feature_grouped_section` (no rewritten body) rather than inserting a
flat `### <category>` block that would land mis-ordered at the section end.
The refusal message names the first dated-topic heading line so the caller
can hand-edit. `cmdChangelogLog` / `add_batch` propagate the refusal (single
op returns it; batch routes each entry to `skipped[]`) and leave
CHANGELOG.md untouched.

Detection requires all three signals, to keep the refusal precise (never
block a legitimate insert): ≥1 `### ` heading, NONE of them a canonical
category word (a single canonical heading → flat layout, handled by the
normal insert + the INV-9 advisory), and ≥1 flush-left `**Bold**` run line.
A normal bullet `- **summary**` trims to a leading `-`, so it never trips
the bold-run signal.

## Test plan

INV-1..5 are exercised behaviourally on the pure `ChangelogLog` helpers.
INV-6/7 drive `cmdChangelogLog` against a seeded temp project
(CHANGELOG.md + ROADMAP.md). INV-8 is source-scrape. INV-9 drives both
the pure helper (malformed vs clean) and the handler (advisory present
vs absent). INV-10 drives `add_from_roadmap` with a >120-char headline
and asserts no ellipsis leaks into the rendered bullet.
