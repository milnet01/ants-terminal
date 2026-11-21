# ANTS-3533 — `changelog_query` read verb (mirror `roadmap_query` over CHANGELOG.md) — review loop log

Record for [`../specs/ANTS-3533-changelog-query-read-verb-mirror.md`](../specs/ANTS-3533-changelog-query-read-verb-mirror.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1** (2 cold lanes: accuracy-vs-code, implementability) —
  CRITICAL 0 · HIGH 3 · MEDIUM 6 · LOW 4 · INFO 0 (verified 13/13). Fixes:
  (H) `canonicalCategories()` is file-private → reuse needs ~2 LoC public
  exposure (§ 2.1/2.3/4); (H) id-extraction regex matched `UTF-8`/`SHA-256` →
  parenthetical-citation-group rule (§ 3, INV-3); (H) §2.2 example was impossible
  (`0.7.100` + `unreleased:true`) → `unreleased` driven by the version token
  only (§ 2.2/3); (M) `ids` threshold written inverted (`≤100`→`>100`); (M)
  `date` `null` vs `""` → always a string (§ 2.2); (M) mode×id/filter combos
  undefined → new § 2.4 combination-semantics + `bad_mode_combo`; (M)
  `text_oneline` only in `headline_only`; (M) default `limit` / soft-cap
  truncation stated; (M) fenced-code state added to § 3; (L) non-canonical `###`
  resets category; (L) version-heading date = trimmed remainder rule; (L) INV-7
  testability reworded; (L) `caller_cwd_required` added to § 2.3.
- **Loop 2** (same 2 cold lanes, no fix-briefing) — CRITICAL 0 · HIGH 3 ·
  MEDIUM 6 · LOW 4 · INFO 0 (verified 13/13). New findings the loop-1 fixes
  exposed: (H) the parenthetical id rule under-extracted the live
  `(ANTS-1335, lane-2 M2 deferral)` mixed group → re-based extraction on the
  **project roadmap prefix `P-NNNN`** (kills `UTF-8`/`SHA-256`/`(SHA-256)` AND
  reads mixed groups, § 3/INV-3); (H) `id`/`ids` result envelope undefined →
  added `matched_ids`/`missing_ids`/`found` + not-found-is-not-a-refusal (§ 2.2,
  INV-5); (H) unterminated-fence rule self-contradictory → `## [` at col 0 is a
  hard reset that closes an open fence (§ 3); (M) `unreleased` "exactly" vs
  "case-insensitive" → case-insensitive + version normalised to `Unreleased`; (M)
  pagination fields + `count`/`total` semantics defined for all list modes; (M)
  version_index category/query = validate-then-ignore, refusal precedence list
  (§ 2.3); (M) fence detection recognises indented body fences; (L) category
  accessor name settled (`canonicalCategories()` hoisted); (L) tab-indented
  continuation; (L) empty/duplicate-version/offset-past-end degenerate cases.
- **Loop 3** (same 2 cold lanes, no fix-briefing) — CRITICAL 0 · HIGH 1 ·
  MEDIUM 6 · LOW 3 · INFO 0 (verified 10/10). Both lanes independently caught the
  same (H): INV-3 still carried the loop-1 "parenthetical citation group" rule
  that § 3 was rebased away from → INV-3 rewritten prefix-keyed. Plus: (M)
  prefix resolution mis-attributed to `roadmap_query` → named the real resolver
  `RoadmapFoldIn::sniffIdPrefix(root,"ANTS")` (`roadmapfoldin.h:81`), added a § 4
  row; (M) `RoadmapDialog::parseShippedDates` is the uncited over-matching
  precedent → § 3/§ 4 note it as the deliberate non-reuse; (M) INV-2 fence
  test-surface vs § 3 column-0 hard-reset reconciled (indented fake heading);
  (M) precedence omitted `no_changelog`/`format_mismatch` → path-resolution
  ordered first (§ 2.3); (M) `id`+`ids` both present undefined → `bad_args`
  (§ 2.3/2.4); (M) prefix-resolution reuse row added (§ 4); (L) empty `### `
  regex → resets category; (L) `version_index.categories` zero-count/order/
  entry_count defined; (L) `P` cache-staleness note (§ 5).
- **Loop 4** (same 2 cold lanes, no fix-briefing) — CRITICAL 0 · HIGH 0 ·
  MEDIUM 3 · LOW 6 · INFO 0 (verified 9/9). Accuracy lane fully clean (every
  citation re-verified; § 3/INV-3 confirmed reconciled). Implementability fixes:
  (M) `query`-over-`body` vs § 8 "lean path stores no prose" inconsistency →
  `body` is ALWAYS parsed (id-scan + query search it); `include_body` gates
  emission only (§ 8); (M) fence "toggle" vs "matching pair" → CommonMark
  close-matching (same char, ≥ length, no info string) (§ 3); (M) `ids`-batch
  `bad_case` undefined → demoted to `missing_ids`, no batch refusal (§ 2.3,
  INV-5); (L) entry/version document-ordering stated; (L) `category` filter is
  case-sensitive; (L) `offset`/`limit` still shape-validated under an `id`
  override; (L) § 2.2 example shows kept `**bold**` markdown; (L) em-dash
  attributed to the `/bump` promote path, not `ChangelogLog`.
- **Loop 5** (same 2 cold lanes, no fix-briefing) — CRITICAL 0 · HIGH 0 ·
  MEDIUM 1 · LOW 2 · INFO 0 (verified 3/3). Accuracy lane clean 2nd consecutive
  loop (zero discrepancies). (M) `include_body` listed `headline_only` in § 2.1
  but § 2.2/INV-4 always drop `body` there → § 2.1 corrected (entries-only); (L)
  `limit` bounds pinned to `1..500` (mirrors `roadmap_query`); (L) a `###`/bullet
  before any `## [` heading is skipped (§ 3). **Converged** — the remaining
  findings were a single stale-word contradiction + two minor bounds (all
  mechanical, no design change); accuracy verified clean twice running.
- **Loop 6** (2026-09-13, ANTS-5071 amendment; review-contract, 3 cold lanes) —
  Q1 7 · Q2 1 · Q3 0 · Q4 2 (verified 10 / dismissed 5; fixed 9 / surfaced 1).
  Fixed: § 2.3 precedence now the handler's order; § 2.2 names the ANTS-3576
  downshift; § 3 fence opener bounded to three spaces, no tab; § 3 dated topic
  notes the writer does not check its date (ANTS-5148); § 3 known limit covers
  any id cited only in a topic's headline or prose (ANTS-5145); INV-2 names the
  dated-topic exception; INV-8 defines a malformed `ids`, with the wrong-type
  refusal unbuilt (ANTS-5146); INV-10's test surface is parser-level and puts
  its negative cases after a canonical `### Added`. Surfaced, not fixed: an
  `id`/`ids` lookup is paged by the code while § 2.4 and INV-5 say it overrides
  pagination (ANTS-5147).
- **Loop 7** (2026-09-13, ANTS-5071 amendment; 3 cold lanes; cap reached) —
  Q1 4 · Q2 2 · Q3 0 · Q4 0 (verified 6 / dismissed 5; fixed 6). Fixed: § 2.2
  `count == total - offset` on an untruncated page; § 2.2 downshift needs no
  `limit`; § 3 signature is the header's `ParseResult` (loop 6 dismissed this;
  INV-10's rollup check needs it); § 3 category heading is a `### ` line or a
  bare `###`; § 3 a `- ` line in a dated topic's prose is an entry; § 7 names
  the test contract update. Dismissed as changing nothing built: allowlist
  naming across § 4, INV-6 and § 7; § 2.1 lacking `path` and `section`; the
  example envelope's `limit`; the `caller_cwd_required` note; `categories` key
  order. At the cap, no tail beyond ANTS-5147, which stays surfaced.
- **Loop 8** (2026-09-13, ANTS-5145 amendment; review-contract, 3 cold lanes) —
  Q1 0 · Q2 2 · Q3 0 · Q4 0 (verified 2 / dismissed 1; fixed 2). Fixed: § 3's
  topic entry body ends at a column-0 `- ` bullet outside a fence, and the
  sentence saying such a line always starts a bullet is gone (all three lanes);
  § 3's topic `text` strips the separator with the whitespace around it, as
  INV-11 expects. Dismissed as changing nothing built: § 2.1 lacks the
  `section` alias § 2.3 names (the handler already accepts it). Three open
  questions resolved with no finding: the Status line's stale ANTS-5147 note, a
  `####` line inside a topic body, and INV-10's rollup count.
- **Loop 9** (2026-09-13, ANTS-5145 amendment; 3 cold lanes; cap reached) —
  Q1 0 · Q2 1 · Q3 0 · Q4 0 (verified 1 / dismissed 2; fixed 1). Fixed: INV-11
  names § 3's exception, so a heading with neither a headline nor prose makes no
  entry. Dismissed as changing nothing built: § 2.1 lacking the `section` alias,
  and the allowlist sets INV-6, § 4, § 7 and the test contract each name. Four
  open questions resolved with no finding (the stale ANTS-5147 note, what
  "outside a fence" covers, `section` handling, a topic heading before any
  version heading); two outside the amendment left unexamined (which resolver
  supplies `P`, `fenceInfoOf`'s indent rule). Cap reached (2 for a spec): calm,
  0 of this loop's 1 finding landed on text loop 8 wrote; 3 of the run's 3
  verified findings fall inside the gated span, the amendment commit 3d204ab0.
  Tail filed: none.
- **amend (2026-10-12, ANTS-5651)** — not a review; no reviewer dispatched. § 2.2's paging sentence now says an explicit `limit` caps the row count and the page is still cut to `softCapBytes()`, per ANTS-1436 INV-9 as amended. Decided by the user 2026-10-11. Gate owed: review-contract on this spec and ANTS-1436.
- **Loop 10 (2026-10-12, ANTS-5651 amendment; review-contract, 2 cold lanes, every lane held every question)** — Q1 2 · Q2 2 · Q3 0 · Q4 0 (verified 4 / dismissed 1; fixed 4). In the amendment: the Status line now says ANTS-5651's cut is not yet built (both lanes). Records of shipped code, fixed under rule 14's code-exists exception: § 2.2's example drops `limit` and `truncated:false`, which the handler never emits, and the sentence about the example's `50` is gone; § 2.3 and INV-5 add `mode:lint` to the `bad_mode_combo` case; INV-6 and § 7 item 3 say three allowlists, as § 4 does. Dismissed as changing nothing built: § 2.1 lacking the `section` alias (the handler already accepts it).
- **Loop 11 (2026-10-12, ANTS-5651 amendment; 2 cold lanes, every lane held every question; cap reached)** — Q1 0 · Q2 0 · Q3 0 · Q4 1 (verified 1 / dismissed 1; fixed 1). Fixed: § 7 item 2 names a test for the explicit-limit cut on this verb's own path, since the allowlist scrape INV-6 names cannot fail on a paging defect. Dismissed as changing nothing built: § 2.1 lacking the `section` alias (the handler already accepts it). Cap reached (2 for a spec): calm, 0 of this loop's 1 finding landed on text loop 10 wrote. Whole run: 2 of 5 verified findings fall inside the gated span, commit 6093600f. This loop's fix was read by no lane. Tail filed on ANTS-5651: INV-6 says paging engages through the dispatch allowlists, though the handler calls `pageBullets` itself; § 2.2's "~20 KB" can be lower under `softCapBytes()`.
- **amend (2026-10-26, ANTS-6282)** — not a review; no reviewer dispatched. § 2.2's release citation now also reads a bullet's indented continuation lines, matching the ids `entries` mode reports; reported by frameledger, whose wrapped bullets drew five false `shipped_not_cited` findings. No gate: it changes no Goal, invariant or invariant test (rules/14-contract-gate.md § The instance).
- **amend (2026-11-13, ANTS-5666)** — not a review; no reviewer dispatched. INV-6 now says the handler pages itself through `PaginationEngine::pageBullets`; § 2.2 names `softCapBytes()` in place of "~20 KB". Both read in `src/remotecontrol_changelog.cpp` and `src/paginationengine.cpp`.
- **amend (2026-11-18, ANTS-6831)** — not a review; no reviewer dispatched. § 2.2's release citation now skips an id straight after "part of" for `cited_not_shipped`, unless the same line names it plainly, matching cut-release step 0f; reported by DeFragR. Read in `src/changelogquery.cpp` (`onlyPartOf`, `releaseLint`).
