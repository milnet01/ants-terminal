# Feature: the roadmap section cache holds no bodies

## Problem

`roadmap_query`'s `section=` path caches each section's bullets in
`RemoteControl::m_roadmapSectionCache`, up to `kRoadmapSectionCacheCap`
sections. Each bullet carried its whole body, and a parent heading's entry
carries its whole subtree, so the cache grew far past the budget in
`docs/specs/ANTS-1346.md` § 4 (ANTS-5094). The user decided on 2026-09-14
to fix the code, not the budget.

## Contract

The cache stores bullets without `body`, `body_truncated` or
`composed_trailers`. A call that needs bodies — `include_body:true`, or a
`query` filter, which matches body text — reads the section afresh and
does not answer from the cache. Every other call may answer from it.

## Invariants

**INV-1 — the cache holds no bodies.** After a `section=` call with
`include_body:true`, the reply carries each bullet's body and the cached
entry for that section carries none.

**INV-2 — bodies still come back on a warm cache.** A second
`include_body:true` call for the same section returns the bodies.

**INV-3 — a body-text query still matches on a warm cache.** After a plain
`section=` call has filled the cache, a `query` whose text appears only in
one bullet's body returns that bullet.
