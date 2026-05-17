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

## Test scope

Source-scrape against `src/remotecontrol.cpp` and
`src/claudeintegration.cpp` for anchor strings and critical helper
calls. Mirrors ANTS-1398 / ANTS-1287 test pattern. A full live-
roundtrip test would require instantiating RemoteControl + MainWindow
+ the MCP socket layer; the per-INV anchor scrape catches drift at
the load-bearing sites.
