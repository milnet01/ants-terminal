# Feature spec: roadmap_log flip/annotate locates an ants-v1 emoji bullet in a GFM-majority (mixed-format) roadmap (ANTS-3561)

Cross-session pain (Vestige / 3D_E-NNNN scheme, 2026-07-17):
`roadmap_query id:"3D_E-0031"` finds the bullet, but
`roadmap_log op:"annotate"|"flip" id:"3D_E-0031"` returns
`bullet_not_found`, and the documented `headline:` fallback also fails.

## Root cause

The Vestige roadmap is **mixed-format**: ~994 GFM task-list bullets
(`- [ ] ...`) plus ~29 ants-v1 emoji bullets (`- 📋 [3D_E-NNNN] **...**`)
appended over time by `roadmap_log op:"append"`. `cmdRoadmapLogFlip` walks
GFM bullets first and only falls back to the ants-v1 native walker when the
GFM walk finds **zero** bullets (`bullets.isEmpty()`). In a mixed file the
GFM walk is non-empty, so the ants-v1 fallback never runs and the emoji
bullet — which `walkGfmBullets` does not recognise (it keys on the
`- [ ]` / `- [x]` checkbox prefix, not `- <emoji>`) — is unreachable by
**any** locator. The read path (`roadmap_query` → `parseBullets`) parses both
shapes, hence the read/write asymmetry.

The `3D_E-` prefix itself is canonical since ANTS-3492 (digit-led but
letter-containing), so this is NOT a `bad_id_format` case — the id passes the
guard and reaches a locator that simply never inspects the emoji bullet.

## Surface

- `roadmap_log op:"flip"  id:"<ants-v1 id>"` in a mixed file → resolves the
  emoji bullet and flips it (`format:"ants-v1"`).
- `roadmap_log op:"annotate" id:"<ants-v1 id>"` in a mixed file → appends the
  note to the emoji bullet.
- `roadmap_log op:"flip" headline:"<verbatim emoji-bullet headline>"` in a
  mixed file (em-dash + parenthetical + trailing period intact) → resolves.
- A GFM bullet in the same file still flips by its `**Bold-ID.**` (the GFM
  path is unchanged).
- A genuinely-absent id still returns `bullet_not_found` (the fallback does
  not manufacture a match).

## Invariants

- **INV-1 / flip emoji bullet by id.** In a mixed GFM+ants-v1 file,
  `op:"flip" id:"3D_E-0031"` returns `{ok:true, format:"ants-v1"}` and swaps
  the emoji. The ants-v1 walker is engaged as a fallback because the GFM
  matcher found no match — not because the GFM walk was empty. Behavioural
  via `cmdRoadmapLogFlipForTest`.
- **INV-2 / annotate emoji bullet by id.** `op:"annotate" id:"3D_E-0031"`
  with a `note` returns `{ok:true}` and appends the note under the emoji
  bullet. Behavioural.
- **INV-3 / flip emoji bullet by verbatim headline.** `op:"flip"
  headline:"Meadow realism A — real PBR ground textures on terrain (replace
  the flat-colour placeholder)."` (em-dash + parenthetical + period)
  resolves the same bullet — the headline fallback the field report found
  broken. Behavioural.
- **INV-4 / GFM path intact (regression).** A `- [ ] **G1.** ...` GFM bullet
  in the same mixed file still flips by `id:"G1"` with `format:"gfm"` — the
  ants-v1 fallback only fires on a GFM zero-match and never shadows a real
  GFM hit. Behavioural.
- **INV-5 / absent id still not found (regression).** A canonical but
  genuinely-absent id (`3D_E-9999`) in the mixed file still returns
  `bullet_not_found` — the fallback resolves real bullets only. Behavioural.
