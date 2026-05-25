# ANTS-1646 — roadmap_query duplicate-ID detector

## Background

A hand-edit of `ROADMAP.md` can append a `- ✅ [ANTS-NNNN]` bullet
without going through `roadmap_log op:"append"` — which means the
`.roadmap-counter` invariant the verb maintains (one ID, one bullet)
is bypassed. The pull-31 prep survey found two active bullets sharing
ANTS-1415 (renumbered to ANTS-1645 in pull 30); the silent failure
mode is "the next session asking for the roadmap picks up one of the
two at random."

ANTS-1646 (a) adds a `duplicate_ids[]` envelope field to
`roadmap_query` so every consuming session sees collisions, and
(b) ships `tools/check-roadmap.sh` as a pre-commit / CI guard that
exits non-zero on collisions. This spec covers (a).

## Invariants

### INV-1 — helper computes per-ID occurrences

`rcComputeDuplicateIds(QJsonArray bullets)` walks every bullet,
skips entries with empty `id` (rollups + narrators), and returns
one entry per ID seen on more than one bullet. Each entry carries
`id` + `occurrences[]`; each occurrence carries `section_slug` +
`status`. First-seen order preserved across the result.

### INV-2 — recompute on every cache fill

The detector runs at every call site that assigns
`m_roadmapCacheBullets`:

1. Full-file refresh after stale-cache wipe.
2. Section_index lazy-fill from a section-mode cache HIT.
3. Full-file lazy-fill from a section-mode cache HIT.

Each populates `m_roadmapCacheDuplicateIds` from
`rcComputeDuplicateIds(m_roadmapCacheBullets)`.

### INV-3 — cache wipe clears descriptors

The stale-cache wipe block (mtime advance or TTL expiry) clears
`m_roadmapCacheDuplicateIds` alongside `m_roadmapIndex` and the
section caches, so the next refresh recomputes against fresh
markdown.

### INV-4 — envelope emits only when non-empty

`duplicate_ids` is emitted on the response envelope only when the
descriptor array is non-empty. A clean roadmap (zero collisions)
keeps the existing envelope shape verbatim — no back-compat hazard
for callers that didn't ask for the field.

### INV-5 — three emission paths

The field is emitted from all three envelope-assembly points:

- `mode:"section_index"` response.
- Section-mode response (when the bullets cache is populated by a
  prior full-file / section_index call; pure section-only first hits
  may stay quiet because the cache was never populated).
- Full-file bullets response.

### INV-6 — MCP descriptor documents the field

The `roadmap_query` tool description in `claudeintegration.cpp`
names the `duplicate_ids[]` field, its shape, and its purpose
(surfacing hand-edited drift past the `.roadmap-counter` guard).

### INV-7 — detector keys only on canonical allocated IDs (ANTS-1688)

`rcComputeDuplicateIds` counts a bullet's `id` only when it matches
the canonical allocated-ID shape `^[A-Za-z][A-Za-z0-9_-]*-<digits>$`
(`RoadmapIndex::isCanonicalId`). The GFM adapter (ANTS-1428)
synthesises 10-char content-hash nonces for ID-less bullets and
surfaces Obsidian `^anchor` tokens; neither is an allocated ID, so
neither can be a `.roadmap-counter` drift collision. Before this
fix a legacy-format roadmap over-reported — e.g. a `35ra39wbn1`
hash surfaced as a 7× "duplicate ID", and the inflated
`occurrences[]` lists drove the `section_index` envelope to ~55 KB
(tripping the persisted-output truncation path). Keying on the
canonical shape removes the over-report and the bulk of the payload.

### INV-8 — occurrences tail capped (ANTS-1688)

Each duplicate entry emits at most `kDuplicateOccurrencesCap` (3)
occurrences; when an ID genuinely collides more than that, the
dropped tail count is recorded in a per-entry `truncated_count`
field. A real collision set can't blow the response-size budget,
and the caller still learns the true multiplicity. Entries at or
under the cap carry no `truncated_count` (envelope shape unchanged
for the common case).

## Test plan

INV-1 through INV-6 and INV-8 are verified by a source-grep against
the implementation, matching the project's feature-conformance style
(no live MCP round-trip needed at this layer — the detector is a
pure function over the bullets array). INV-7's predicate
(`RoadmapIndex::isCanonicalId`) lives in the Qt-Core-only
`ants_core_lib` and is verified behaviourally (accept allocated IDs,
reject hash nonces / anchors / hyphen-less bold IDs / empties).
