# Feature spec: `roadmap_query` section-index mode (ANTS-1437)

Discoverability companion to ANTS-1287's section mode. The new
`mode:"section_index"` arg returns a compact section index — `{slug,
headline, level, active_count, shipped_count, total_count}[]` — and
no bullets, so callers can discover available slugs without round-
tripping a full bullet payload.

## Invariants

- **INV-1 / default mode is unchanged.** Default (no `mode` arg)
  and explicit `mode:"bullets"` produce envelopes with byte-identical
  shape modulo the `mode` echo (only present when caller passed
  arg). Anchor: `ANTS-1437-INV-1` in `src/remotecontrol.cpp`.
- **INV-2 / unknown mode rejected with hygiene.** `mode:"foo"`
  returns `{ok:false, code:"bad_mode"}` with the 64-byte + control-
  char-`?` verbatim hygiene already used by `bad_status` /
  `bad_section`. Anchor: `ANTS-1437-INV-1/2`.
- **INV-3 / mode + section is bad_mode_combo.**
  `mode:"section_index"` with non-empty `section` returns
  `{ok:false, code:"bad_mode_combo"}`. Anchor: `ANTS-1437-INV-3`.
- **INV-4 / every indexed section is emitted.** Sections with zero
  bullets get `total_count:0` — discoverability beats output size.
  Anchor: `ANTS-1437-INV-4`.
- **INV-5 / counts match bullet-mode result.** For any slug S:
  `active_count` equals `len(roadmap_query{section:S,
  status:"active", include_section_headers:false}.bullets)`. (Loop
  in the implementation uses the same plannedEmoji/progressEmoji/
  doneEmoji predicates as the bullet-mode filter.)
- **INV-6 / rollup bullets excluded from counts.** A rollup
  bullet (empty id + empty headline; ANTS-1398) does NOT contribute
  to its section's total_count, regardless of
  `include_section_headers`. Anchor: `INV-6 rollup`.
- **INV-7 / status filter is no-op for section_index emission.**
  `mode:"section_index"` + `status:"active"` emits the same envelope
  as `status:"all"`. Both echo `filter:"<as supplied>"`. (Implicit —
  counts are pre-categorised.)
- **INV-8 / `unrecognised_format` gate applies before emission.**
  `code:"unrecognised_format"` fires in section_index mode under the
  same conditions as bullet mode. Anchor: `ANTS-1437-INV-8`.
- **INV-9 / section_slug populated on every cache-fill path
  (ANTS-1442 regression).** The section_index tally walks
  `m_roadmapCacheBullets` keyed by `section_slug`. Because the cache
  is shared across `roadmap_query` modes, EVERY cache-fill loop must
  emit `section_slug` — otherwise a section_index call following a
  bullet-mode call walks objects keyed to "" and rolls up zero for
  every section. Three loops in `cmdRoadmapQuery` (bullet-mode pre-
  fill, section_index lazy-fill, full-file lazy-fill); test asserts
  loop-count == section_slug-emission-count. Anchor: `ANTS-1442`.
- **INV-10 / parent sections roll up descendant counts
  (ANTS-1442 root cause).** ROADMAP.md nests bullets under level-3
  headings beneath level-2 version anchors. The tally must walk a
  parent's subtree, not just its immediate bullets — otherwise a
  level-2 section like `0.7.92 — ...` surfaces `0/0/0` while its
  child level-3 sections clearly have content. Per-section emitted
  counts equal the sum of the section's own direct-bullet tally
  PLUS the direct-bullet tallies of every descendant section
  (one whose `[lineStart, lineEnd)` is nested inside the parent's).
  Pure helper: `RoadmapIndex::rollupCounts(index, direct)`. Anchor:
  `ANTS-1442` in `src/roadmapindex.cpp`.
- **INV-11 / ID-only parallel counts + top-level legacy hint
  (ANTS-1622).** Each emitted section carries
  `active_count_id_only` / `shipped_count_id_only` /
  `total_count_id_only` beside the emoji-only counts, so a caller
  sees whether a section's bullets survive the default `bullets[]`
  ID-filter predicate. When ≥1 section's direct bullets all lack a
  `[PROJ-NNNN]` id, the envelope adds a top-level
  `legacy_format_sections[]` array + `legacy_format_hint`. Both stay
  absent on a well-tagged roadmap. Anchor: `ANTS-1622`.
- **INV-12 / `status` shapes section_index emission (ANTS-1848).**
  `status:"active"` drops sections whose `active_count_id_only` is 0;
  `status:"shipped"` drops sections whose `shipped_count_id_only` is
  0; the default `status:"all"` emits every indexed section. Keeps
  the kept set aligned with the default `bullets[]` predicate and is
  the lean planning call. Anchor: `ANTS-1848`.
- **INV-13 / per-section `legacy_format` flag (ANTS-1714b).** A
  section object whose direct (un-rolled) bullets all lack
  `[PROJ-NNNN]` ids (`self.total > 0 && self.totalWithId == 0`)
  carries `legacy_format: true`, mirroring the top-level
  `legacy_format_sections[]` array (INV-11) so a caller reading one
  section object knows it is ID-less without grepping the slug
  against the top-level list. Absent on well-tagged sections.
  Anchor: `ANTS-1714b`.
- **INV-14 / section_index paginates + auto-truncates (ANTS-1729).**
  `mode:"section_index"` now ACCEPTS `offset`/`limit` (superseding the
  ANTS-1436 rejection) and routes its `sections[]` array through
  `PaginationEngine::pageBullets`: when `limit` is omitted the array is
  measure-cut under the ~20 KB soft cap; explicit `offset`/`limit` page
  through it. The envelope carries `offset`/`limit`/`total`/`truncated`
  (+ `next_offset` when truncated) under the same
  `shouldEmitPaginationFields` opt-in as the bullets path.
  `legacy_format_sections[]` stays the full-roadmap hint (not per-page).
  Anchor: `ANTS-1729`.

## Test scope

Source-scrape against `src/remotecontrol.cpp` and
`src/claudeintegration.cpp` for anchor strings and critical helper
calls. Mirrors ANTS-1398 / ANTS-1287 test pattern. INV-10 also
exercises the pure `RoadmapIndex::rollupCounts` helper with a
hand-built fixture — the helper is testable without instantiating
RemoteControl + MainWindow.
