# Feature spec: roadmap_log flip/annotate bullet_not_found suggestions rank by sibling-id prefix for an ID locator (ANTS-3566)

Observed while reproducing ANTS-3561:
`roadmap_log op:"annotate" id:"3D_E-0031"` on a genuinely-absent id returned
three `suggestions[]` all starting with "3" — none an actual `3D_E-` sibling.

## Root cause

Both `bullet_not_found` suggestion rankers in `cmdRoadmapLogFlip` — the GFM
step-7 block and the ants-v1 zero-match block — fall back to the locator
STRING as the ranking needle: `needle = locHeadline.isEmpty() ? locId : ...`.
When the locator is an **id** (no headline), the ranker scores candidate
HEADLINES by token/shared-prefix overlap with the id string. An id like
`3D_E-0031` then matches every headline that merely starts with the same
leading character(s) (e.g. any title starting "3"), surfacing unrelated
bullets while the real same-project siblings (`3D_E-0010`, `3D_E-0011`) —
which share no headline text — are never suggested.

## Fix

When the locator is an id (`!locId.isEmpty()`), rank sibling **ids** by
shared case-insensitive prefix with the locator id instead of ranking
headlines. Candidates that share no id prefix are dropped, so when nothing
is related the suggestion list is simply empty rather than misleading. The
headline-locator path (`headline:`) is unchanged — it still ranks by token
overlap against the canonical headline (ANTS-3378). Applies to both the GFM
and the ants-v1 zero-match blocks. Shared via the `rcRankIdsBySharedPrefix`
file-local helper.

`op:"amend_body"` is out of scope: its zero-match refusal carries no
`suggestions[]` array, so there is nothing to mis-rank there.

## Surface

- `op:"flip"|"annotate" id:"<absent id>"` (ants-v1 file) → `suggestions[]`
  contains only bullets whose id shares a prefix with the locator; a bullet
  whose only overlap is a headline character is not suggested.
- Same for a GFM-majority file (bold-ID bullets).
- A `headline:` locator still ranks by headline token overlap (regression).

## Invariants

- **INV-1 / ants-v1 id-locator suggestions are siblings.** In an ants-v1
  file with `[3D_E-0010]`, `[3D_E-0011]` and an unrelated `[OTHER-0001]`
  whose HEADLINE starts with "3", `op:"flip" id:"3D_E-0031"` (absent)
  returns `bullet_not_found` whose `suggestions[]` all have `id` beginning
  `3D_E-` and never include `OTHER-0001`. Behavioural via
  `cmdRoadmapLogFlipForTest`.
- **INV-2 / GFM id-locator suggestions are siblings.** In a GFM file with
  bold-IDs `AX10`, `AX11` and an unrelated `ZZ01` whose headline starts
  "ax", `op:"flip" id:"AX99"` (absent) returns suggestions whose ids all
  begin `AX` and never include `ZZ01`. Behavioural.
- **INV-3 / headline locator ranking unchanged (regression).** A
  `headline:` locator miss that shares a token with a real bullet still
  surfaces that bullet in `suggestions[]` — the id branch does not disturb
  the headline path. Behavioural.
