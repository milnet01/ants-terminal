# Feature spec: roadmap_log op:amend_body + op:flip_batch locate ants-v1 emoji bullets in a GFM-majority (mixed-format) roadmap (ANTS-3565)

Follow-up to ANTS-3561, which fixed the SAME blind spot in `op:flip` /
`op:annotate` (`cmdRoadmapLogFlip`). Two sibling verbs still choose ONE
format for the whole file and so cannot reach an ants-v1 emoji bullet
(`- 📋 [3D_E-NNNN] **...**`) appended into a GFM-majority roadmap:

- `cmdRoadmapLogAmendBody` walks GFM first and only tries the ants-v1
  walker when the GFM walk is **empty**. In a mixed file the GFM walk is
  non-empty, so an id/headline locator that names the emoji bullet returns
  `bullet_not_found`.
- `cmdRoadmapLogFlipBatch` picks `isGfm = !walkGfmBullets().isEmpty()`
  and runs the whole locators array against that one set — the appended
  emoji bullets are never walked.

This is the real Vestige `3D_E-NNNN` shape: ~994 `- [ ]` GFM checkboxes plus
~29 appended `- 📋 [ID]` emoji bullets. The read path (`roadmap_query` →
`parseBullets`) parses both, so the emoji bullets are visible but
unaddressable by these two write verbs — the same read/write asymmetry
ANTS-3561 fixed for flip/annotate.

## Fix

- **amend_body**: on a GFM zero-match for an id/headline locator (anchor is
  GFM-only), fall back to the ants-v1 walker before refusing — mirror
  `cmdRoadmapLogFlip`'s step-7 fallback. A single ants-v1 match sets the
  body-span anchor + `format:"ants-v1"` and the shared patch path continues.
- **flip_batch**: walk BOTH `gbs` and `vbs` up front. Per locator, resolve
  against GFM first; on a GFM zero-match for an id/headline locator, fall
  back to the ants-v1 set. Each `Target` records whether it is an ants-v1
  bullet so Phase-2 applies the matching surgery (`applyAntsV1Flip` vs
  `applyGfmFlip`). A flipped emoji-bullet result entry carries
  `format:"ants-v1"`.

Ambiguous GFM (>1 match) and anchor locators keep their existing GFM-only
behaviour — the fallback fires only on a GFM zero-match, so it never shadows
a real GFM hit.

**ANTS-3570 follow-up.** The original fix excluded `line_range` from the
ants-v1 fallback ("a line_range legitimately means GFM rows"), leaving
Vestige's `3D_E-NNNN`-by-line repro unaddressable: an emoji bullet appended
into a GFM-majority roadmap sits on a line no GFM row occupies, so a range
naming it matched zero GFM rows and returned `bullet_not_found`. The fallback
now also walks the ants-v1 set for a line in-range on a GFM zero-match — anchor
stays GFM-only, and a range that hits real GFM rows is served by them first.

## Surface

- `roadmap_log op:"amend_body" id:"<ants-v1 id>" old_text new_text` in a
  mixed file → patches the emoji bullet's body (`format:"ants-v1"`).
- `roadmap_log op:"amend_body" headline:"<verbatim headline>"` → same.
- `roadmap_log op:"flip_batch"` with a locator naming the emoji bullet →
  flips it; its `flipped[]` entry carries `format:"ants-v1"`.
- A GFM bullet in the same file still amends / flips (GFM path unchanged).
- A genuinely-absent id still fails (`body_match`/`bullet_not_found` for
  amend_body; `skipped[]` bullet_not_found for flip_batch).

## Invariants

- **INV-1 / amend_body emoji bullet by id.** In a mixed GFM+ants-v1 file,
  `op:"amend_body" id:"3D_E-0031"` with a body-local `old_text` returns
  `{ok:true, format:"ants-v1"}` and the file shows the replacement. The
  ants-v1 walker engages as a fallback because the GFM matcher found no
  match — not because the GFM walk was empty. Behavioural via
  `cmdRoadmapLogAmendBodyForTest`.
- **INV-2 / amend_body emoji bullet by verbatim headline.**
  `op:"amend_body" headline:"<em-dash + parenthetical + period headline>"`
  resolves the same bullet and patches it. Behavioural.
- **INV-3 / flip_batch emoji bullet by id.** `op:"flip_batch"` with a
  single locator `id:"3D_E-0031"` returns `flipped_count:1` and the entry
  carries `format:"ants-v1"` with the swapped emoji. Behavioural via
  `cmdRoadmapLogFlipBatchForTest`.
- **INV-4 / flip_batch mixed targets in one commit.** A batch of two
  locators — a GFM bullet by `id:"G1"` and the emoji bullet by
  `id:"3D_E-0031"` — flips both (`flipped_count:2`); the GFM entry has no
  `format` field, the emoji entry carries `format:"ants-v1"`. Behavioural.
- **INV-5 / GFM paths intact (regression).** `op:"amend_body" id:"G1"`
  still patches the GFM bullet's body (`format:"gfm"`), and a GFM-only
  `flip_batch` still flips by bold-ID — the ants-v1 fallback never shadows a
  real GFM hit. Behavioural.
- **INV-6 / absent id still fails (regression).** `op:"amend_body"
  id:"3D_E-9999"` returns a non-ok envelope (no match manufactured), and a
  `flip_batch` locator with an absent id lands in `skipped[]` with
  `bullet_not_found`. Behavioural.
- **INV-7 / flip_batch emoji bullet by line_range (ANTS-3570).** In a mixed
  GFM+ants-v1 file, `op:"flip_batch"` with a single `line_range:[N,N]` locator
  where N is the emoji bullet's 1-based line returns `flipped_count:1`, the
  entry carries `format:"ants-v1"`, and the file shows the swapped emoji. The
  range matches zero GFM rows, so the ants-v1 fallback resolves it.
  Behavioural.
