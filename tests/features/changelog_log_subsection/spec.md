# Feature: `changelog_log` op:`add_subsection` (ANTS-3584)

Test contract for the dated feature-grouped subsection writer. `changelog_log`
gains an opt-in `op:"add_subsection"` that emits a
`### <date> <Category> — <headline>` block at the TOP of `## [Unreleased]`
(newest-first) — for CHANGELOGs grouped by dated topic (the Vestige/3D_Engine
house style) rather than flat Keep-a-Changelog categories. The flat
`add` / `add_from_roadmap` / `add_batch` / `normalize` paths are unchanged.

Pure-helper INV-1..6 exercise `ChangelogLog::insertUnreleasedSubsection`
directly; behavioural INV-7..9 drive `RemoteControl::cmdChangelogLog`
(`m_main`-independent, null `RemoteControl`) over a `QTemporaryDir` CHANGELOG.
Mirrors the `changelog_log_normalize` harness.

## Invariants under test

- **INV-1** — a dated subsection is inserted at the TOP of `[Unreleased]`:
  heading `### <date> <Category> — <headline>`, then the flush-left prose
  `body`, then each bullet as `- **summary** (id)`. Section content below is
  untouched.
- **INV-2** — a non-canonical `category` → `bad_category` refusal, no markdown.
- **INV-3** — a body with no `## [Unreleased]` heading → `not_unreleased`.
- **INV-4** — newest-first: with an existing dated subsection present, the new
  block lands ABOVE it (between `## [Unreleased]` and the existing `### `), and
  the existing subsection + its bullets are preserved.
- **INV-5** — a `## [Unreleased]` with no blank spacer before its first content
  gets one repaired, so the heading never abuts the inserted block.
- **INV-6** — empty `body` and no bullets → the block is just the heading + a
  blank line (well-formed, no stray blanks).
- **INV-7** — handler write path: `op:"add_subsection"` rewrites CHANGELOG.md
  with the block and returns `{ok, heading, date, category, line,
  bytes_written}`.
- **INV-8** — handler `dry_run`: nothing is written; response carries
  `dry_run:true`, `written:false`, and the would-be `heading`/`line`/`bytes`.
- **INV-9** — handler guards: absent `headline` → `missing_field`; a bad
  `category` → `bad_category`; `category` omitted but `kind` supplied → derived
  (e.g. `kind:"fix"` → `Fixed`).

## Pre-fix check

Against pre-implementation source `ChangelogLog::insertUnreleasedSubsection`
does not exist (compile error) and `cmdChangelogLog` rejects
`op:"add_subsection"` with `bad_op_combo`, so every assertion fails. Verified
before wiring the implementation.

Label: `features;fast`.
