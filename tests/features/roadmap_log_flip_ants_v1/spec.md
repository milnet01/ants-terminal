# Feature spec: roadmap_log op:"flip" for ants-v1 native format (ANTS-1441)

ANTS-1428 shipped `op:"flip"` for GFM-task-list bullets only.
Flipping ANTS-1442 to ✅ from a live MCP session failed with
`unrecognised_format` because Ants's own roadmap is ants-v1 native
(uses `[ANTS-NNNN]` bracket IDs, not GFM checkboxes). ANTS-1441
adds a parallel ants-v1 native path so the flip works on the
project's own roadmap.

## Invariants

- **INV-1 / walker recognises ants-v1 bullet shape.**
  `walkAntsV1Bullets` accepts lines matching
  `^- (✅|📋|🚧|💭) \[<prefix-NNNN>\] <headline>...` and skips
  fenced code blocks. ANTS-2051 — the bracket-ID leading letter is
  case-insensitive (`[A-Za-z][A-Za-z0-9_-]*-\d{1,8}`), mirroring the
  read path's shared `idTokenPattern()`, so lowercase project prefixes
  like `[mame-curator-1065]` are recognised and flippable. The prior
  uppercase-only `[A-Z]…` form left markerless ants-v1 roadmaps that
  `roadmap_query` reads fine effectively read-only to `roadmap_log`
  (MAME Curator HIGH, cross-session report 2026-06-10).
- **INV-2 / flip is GFM-first, ants-v1 fallback.** When
  `walkGfmBullets` returns ≥ 1 bullet, the GFM path runs (no
  behaviour change from ANTS-1428). When GFM returns zero AND
  `walkAntsV1Bullets` returns ≥ 1, the ants-v1 path runs.
  Neither path → `unrecognised_format` refusal (existing behaviour
  with refreshed error message naming both formats).
- **INV-3 / `anchor` locator rejected on ants-v1.** Caret anchors
  are GFM-specific. ants-v1 with `anchor:` arg → `bad_op_combo`
  refusal naming `id` or `headline` as the alternatives.
- **INV-4 / `id` locator matches `[<id>]` bracket directly.**
  No `**Bold-ID.**` token lookup needed; the bracket IS canonical.
- **INV-5 / `headline` locator hashes via `rcFnv1a64 +
  rcNormaliseHeadline`.** Same predicate as GFM (shared helpers).
- **INV-6 / no anchor injection, no counter consumption.** ants-v1
  bullets already carry the canonical ID; the flip is a pure
  status-emoji swap. `applyAntsV1Flip` replaces the emoji byte
  sequence at position 2 in the line; no other surgery.
- **INV-7 / fenced bullets refused.** Same as GFM: a bullet inside
  ```` ``` ```` blocks gets `anchor_unsafe_context` refusal.
- **INV-8 / success envelope carries `format:"ants-v1"`.** Lets
  callers distinguish which path ran. Other envelope fields:
  `ok:true, op:"flip", from_status, to_status, file, line,
  bytes_written, anchor_injected:false, id`.

## Test scope

Source-scrape against `src/remotecontrol.cpp` for the walker,
applier, and the cmdRoadmapLogFlip integration anchors. Behaviour
parity with ANTS-1428 (GFM path) is preserved by ordering: GFM
runs first and only falls through on zero matches.
