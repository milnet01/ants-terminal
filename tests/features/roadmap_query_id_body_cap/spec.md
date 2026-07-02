# ANTS-3425 — roadmap_query by-id `max_body_bytes` honoured end-to-end

## Problem

`roadmap_query` caches every bullet's `body` field in
`m_roadmapCacheBullets`. A targeted `id` / `ids` fetch may raise the body
cap via `max_body_bytes` (ANTS-3402): `cmdRoadmapQuery` parses it into
`idBodyCap` (clamped `[2000, 16384]`) and the id/ids branches call
`rcCapBodyFields(matches, idBodyCap)`.

But `rcCapBodyFields` only *shrinks* (`body.left(cap)` when the body is
longer than `cap`). ANTS-3402 raised the cache STORE cap to
`kRoadmapQueryBodyStoreCap` (16384) in `rcBuildBulletCacheArray` only — the
three INLINE builders that populate `m_roadmapCacheBullets` on the common
paths (full-file pre-fill, section_index lazy-fill, full-file lazy-fill
after a section hit) still called `rcSetBodyFields(o, b.body)` with the
default 2000 cap. So the cached body was pre-truncated to 2000, and a
larger `max_body_bytes` could never restore it — the arg was inert. A
caller after a large epic's full body got it cut at ~2000 chars and had to
fall back to a full-file Read of the 2 MB ROADMAP.md.

## Fix

Every builder that populates `m_roadmapCacheBullets` stores the body at
`kRoadmapQueryBodyStoreCap`, matching `rcBuildBulletCacheArray`. List and
section emission still re-truncate to the 2000 list cap at emission time
(`rcCapBodyFields(page.slice, kRoadmapQueryBodyCap)`), so only the opt-in
id/ids fetch sees the larger body. RAM is bounded by the roadmap file size
(Σ bodies ≤ the file), the bound ANTS-3402 already accepts.

## Invariants

### INV-1 — id fetch honours max_body_bytes

`cmdRoadmapQuery({id, max_body_bytes: 6000, include_body: true})` against a
bullet whose body exceeds 2000 chars returns a `body` longer than the 2000
default cap (up to the requested 6000).

### INV-2 — ids[] fetch honours max_body_bytes

The plural `ids:[...]` path applies the same raised cap: a matched bullet's
returned `body` exceeds 2000 chars.

### INV-3 — list emission still caps at 2000

An unfiltered list fetch (no `id`/`ids`, no `max_body_bytes`) still returns
each `body` truncated to the 2000 list cap with `body_truncated: true` —
the raised store cap is reserved for the targeted fetch.

## Test plan

End-to-end against a seeded temp ROADMAP.md (a bullet with a ~3000-char
body) driving `RemoteControl::cmdRoadmapQuery` live (path resolved from
`caller_cwd`, `m_main`-independent). INV-1/2 assert the returned body
exceeds 2000; INV-3 asserts a plain list fetch stays capped at 2000. The
test FAILS against pre-fix code (cached body pre-truncated to 2000, so the
raised cap is inert) and passes after. This is the end-to-end coverage
ANTS-3402's source-scrape test missed.
