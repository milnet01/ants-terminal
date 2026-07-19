# Feature: changelog_query (ANTS-3533)

Read-only MCP verb: a structured Keep-a-Changelog reader over CHANGELOG.md,
mirroring `roadmap_query`. Full contract: `docs/specs/ANTS-3533.md`.

This test asserts the pure `ChangelogQuery::parse` parser (the net-new logic)
and source-scrapes the handler wiring.

## Parser invariants exercised (docs/specs/ANTS-3533.md § 3, INV-2/3)

- **INV-2** — recognises `## [<version>]` headings (em-dash / hyphen / no
  separator; bare `## [Unreleased]` → `unreleased=true` + version normalised),
  `### <Category>` sub-headings (non-canonical `###` resets category), `- `
  bullets + ≥2-space/tab continuation; a fenced block suppresses structure, but
  a column-0 `## [` inside an unterminated fence is a hard reset.
- **INV-3** — id extraction returns every `<P>-NNNN` token (P = roadmap prefix)
  in text+body joined, document order, deduped — covering trailing / multiple /
  mid-bold / mixed-parenthetical placements — and excludes `UTF-8` / `SHA-256` /
  `(SHA-256)` (prefix ≠ P).
- Degenerate inputs (§ 3): empty changelog → no entries; a bullet before any
  version/category heading is skipped; per-version category rollup omits
  zero-count categories in canonical order.

## Wiring (source-scrape, INV-1/6/9)

- `changelog_query` registered via `rcDelegate(&RemoteControl::cmdChangelogQuery)`.
- Present in all four opt-in allowlists: `isFieldProjectionTool`,
  `isOffloadEligible`, `isEtagSupportedTool`, `callerCwdContractFor` (Required).
- `canonicalCategories()` is public (hoisted for the `bad_category` echo).
