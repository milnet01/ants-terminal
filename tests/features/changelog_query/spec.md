# Feature: changelog_query (ANTS-3533)

Read-only MCP verb: a structured Keep-a-Changelog reader over CHANGELOG.md,
mirroring `roadmap_query`. Full contract: `docs/specs/ANTS-3533.md`.

This test asserts the pure `ChangelogQuery::parse` parser (the net-new logic)
and source-scrapes the handler wiring.

## Parser invariants exercised (docs/specs/ANTS-3533.md § 3, INV-2, INV-3, INV-10)

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
  bullets are entries counted in that version's rollup. An invalid date or a
  non-canonical word resets the category: after an `### Added` bullet, the
  bullets under such headings yield no entry.
- Degenerate inputs (§ 3): empty changelog → no entries; a bullet before any
  version/category heading is skipped; per-version category rollup omits
  zero-count categories in canonical order.

## Wiring (source-scrape, INV-1/6/9)

- `changelog_query` registered via `rcDelegate(&RemoteControl::cmdChangelogQuery)`.
- Present in the opt-in allowlists (`fields=` needs none since ANTS-4524 —
  every verb honours it): `isCompactArgTool`,
  `isOffloadEligible`, `isEtagSupportedTool`, `callerCwdContractFor` (Required).
- `canonicalCategories()` is public (hoisted for the `bad_category` echo).
