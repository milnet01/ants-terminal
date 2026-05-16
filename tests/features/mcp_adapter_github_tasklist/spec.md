# mcp_adapter_github_tasklist — feature contract

Tier 1 (read-side adapter) tests for ANTS-1428. See
`docs/specs/ANTS-1428.md` for the full design + rationale; this
is the test-side mirror for the read path. Tier 2 (write-side
caret-anchor surgery) and Tier 3 (dialog renderer fork) ship in
the next bundle and have their own test files.

## What this test guards

The GFM-task-list adapter branch in
`RoadmapDialog::parseBullets`:

- **INV-1 / Read-side detection.** No `<!-- ants-roadmap-format:
  1 -->` marker AND ≥ 1 `^- \[[ x]\]` line in first 100 non-
  empty lines → adapter engages. Marker present → native parser
  regardless of content.
- **INV-2 / Bold-ID preservation.** A GFM bullet that begins
  with a `**Bold-ID.**` token reports that token as
  `BulletRecord.id` with `synthetic == false`. Multi-prefix
  projects (`Sh4`, `Ed1`, `VEST-0042`) all work.
- **INV-3 / Synthetic-ID stability.** A GFM bullet with no
  bold-ID token reports a content-hash-derived ID
  (`synthetic == true`). The ID is stable across line
  reorders; FNV-1a 64-bit gives effectively zero collisions
  at document scale.
- **INV-4 / Status mapping.** `- [x]` → `status:"✅"`,
  `- [ ]` → `status:"📋"`, inline emoji prefix wins
  (including the contradictory case `- [x] 📋 ...` → `📋`).
- **INV-5 / Section completion inheritance.** A `## Heading
  (COMPLETE)` or `### Heading - done` section causes its
  enclosed planned-state bullets to inherit ✅ (unless an
  inline emoji overrides).

## Bundle

`test_claude` — joins the sibling `parseBullets` tests
(`roadmap_parser_blank_line_continuation`,
`mcp_roadmap_unrecognised_format`).
