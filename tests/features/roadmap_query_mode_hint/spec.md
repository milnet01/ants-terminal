# roadmap_query_mode_hint — a bad_mode refusal names the id/ids route (ANTS-4511)

## Problem

`mode:"by_id"` is the obvious first guess for looking up one bullet. It
refuses with `bad_mode` and an `accepted` array — good behaviour, and
already the ANTS-3617 contract — but none of the five accepted names says
*this is how you fetch one item by id*, because that route is an
**argument** (`id` / `ids[]`) and not a mode at all.

So the refusal is correct and unhelpful at the same time: it answers
"that is not a mode" and withholds "here is what you actually want". One
wasted round-trip on plausibly the most common roadmap lookup, recurring
for every new session that guesses the same way. Reported by DOOM Ants;
the store making per-item queries the recommended pattern is what made it
worth closing.

## Surface

- `src/remotecontrol_roadmap_query.cpp` — the `kModes` validation block in
  `cmdRoadmapQuery`; adds `hint` beside the existing `accepted`.

## Cases

| # | Asserts |
|---|---------|
| G1 | `mode:"by_id"` still refuses `bad_mode`, still carries `accepted`, and now carries a `hint` naming both `id` and `ids`. |
| G2 | The hint is a property of `bad_mode`, not of the string `"by_id"` — `by-id`, `byId`, `single` and `item` all get it. |
| G3 | A valid mode (`headline_only`) carries **no** hint. |

**Behavioural, not a source-scrape, deliberately.** The sibling
`roadmap_query_id_body_cap` exists because ANTS-3402 shipped with
scrape-only coverage and the feature was inert — the cached body stayed
pre-truncated, so the raised cap did nothing and the scrape passed
anyway. A scrape for the hint string would pass on a hint built into a
branch that never runs.

**Run red before trusting these.** Verified 2026-08-19 against the
pre-fix verb: G1 and G2 failed on the absent hint, and **G3 passed on
both versions** — which is what shows the red run was measuring the hint
rather than a broken harness.

## Would break this

- Special-casing the hint to the `by_id` spelling. G2 is the guard, and
  it is the whole design point: a hint that fires only on the closest
  guess helps only the caller who needed it least.
- Attaching the hint to the success envelope "for discoverability" — G3
  fails, and a hint on a call that was fine reads as a warning.
- Asserting only that `by_id` refuses. The pre-fix verb refuses too; the
  refusal was never the defect.
- Dropping `accepted` once `hint` exists. They answer different
  questions — what a mode IS, and what to use when you wanted something
  that is not one.
