# Feature: changelog_query (ANTS-3533)

Read-only MCP verb: a structured Keep-a-Changelog reader over CHANGELOG.md,
mirroring `roadmap_query`. Full contract: `docs/specs/ANTS-3533.md`.

This test asserts the pure `ChangelogQuery::parse` parser (the net-new logic)
and source-scrapes the handler wiring.

## Parser invariants exercised (docs/specs/ANTS-3533.md § 3, INV-2, INV-3, INV-10, INV-11)

- **INV-2** — recognises `## [<version>]` headings (em-dash / hyphen / no
  separator; bare `## [Unreleased]` → `unreleased=true` + version normalised),
  `### <Category>` sub-headings (any other non-canonical `###` resets category;
  a dated topic heading is INV-10's), `- `
  bullets + ≥2-space/tab continuation; a fenced block suppresses structure, but
  a column-0 `## [` inside an unterminated fence is a hard reset.
- **INV-3** — id extraction returns every `<P>-NNNN` token (P = roadmap prefix)
  in text+body joined, document order, deduped — covering trailing / multiple /
  mid-bold / mixed-parenthetical placements — and excludes `UTF-8` / `SHA-256` /
  `(SHA-256)` (prefix ≠ P).
- **INV-10** — a dated topic heading `### <YYYY-MM-DD> <Category> — <headline>`
  sets the category when the word after a valid date is canonical, so its
  bullets are entries counted in that version's rollup, beside the topic's own
  entry. An invalid date or a non-canonical word resets the category: after an
  `### Added` bullet, the bullets under such headings yield no entry.
- **INV-11** — such a heading is also an entry of its own, ahead of its
  bullets: `text` is the headline with the separator stripped, `body` its
  flush-left prose, and its `ids` come from both. An unparseable date, or a
  heading with neither a headline nor prose, makes no entry.
- Degenerate inputs (§ 3): empty changelog → no entries; a bullet before any
  version/category heading is skipped; per-version category rollup omits
  zero-count categories in canonical order.

## Handler refusals (INV-8)

- **ANTS-5146** — a present `id` or `ids` of the wrong JSON type (a number, a
  boolean, an object) refuses `bad_args` instead of returning the full list.

## Handler id lookup (INV-5)

- **ANTS-5147** — an `id` lookup returns every entry citing the id whatever
  `offset` or `limit` says, with `total` equal to that count and no
  truncation.

## The format check (ANTS-5543, `mode:"lint"`)

- **`ChangelogQueryLint.Ants5543EachShapeIsOneFinding`** — one fixture files
  each kind once, in line order, and leaves alone the preamble, a continuation
  line, one `**Theme:**` line, a dated topic's prose, a note in a section with
  no category, and — inside flat category blocks, where losing the exemption
  would fire — an HTML comment, a fenced block and a link reference.
- **`ChangelogQueryLint.Ants5543VersionNarrowsBothWays`** — `version` checks
  one section, and `unreleased` matches `Unreleased`.
- **`ChangelogQueryHandler.Ants5543LintModeIsReadOnly`** — the verb returns
  the findings, leaves the file byte-identical, and carries `findings:[]` on a
  clean file.

Mutations run 2026-09-28 (rebuilt, run, restored, byte-compared), each
killed: drop the Theme exemption; report placeholder prose immediately; drop
the link-reference, comment or fence skip (these three SURVIVED until the
fixture moved those shapes into flat category blocks); never check order;
treat a dated topic as flat; ignore `version`; never report prose in a
category.

## Wiring (source-scrape, INV-1/6/9)

- `changelog_query` registered via `rcDelegate(rc, &RemoteControl::cmdChangelogQuery)`.
- Present in the opt-in allowlists (`fields=` needs none since ANTS-4524 —
  every verb honours it): `isCompactArgTool`,
  `isOffloadEligible`, `isEtagSupportedTool`, `callerCwdContractFor` (Required).
- `canonicalCategories()` is public (hoisted for the `bad_category` echo).
