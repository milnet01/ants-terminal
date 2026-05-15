# ANTS-1346 — `roadmap_query` section cache LRU eviction

Source-grep + structural tests locking in the LRU policy added to
`RemoteControl::m_roadmapSectionCache`. See `docs/specs/ANTS-1346.md`
for full design.

## Invariants exercised

- **INV-1** Cap declared as `static constexpr int
  kRoadmapSectionCacheCap = 64` in `remotecontrol.h`.
- **INV-2** `m_roadmapSectionLru` exists as a `QList<QString>` member
  alongside `m_roadmapSectionCache`.
- **INV-4** mtime-stale wipe block at `remotecontrol.cpp` clears
  `m_roadmapSectionCache`, `m_roadmapSectionLru`, AND
  `m_roadmapIndex` — three structures in lock-step.
- **Hit-path bump** — the read branch at the `contains(sec->slug)` test
  calls `m_roadmapSectionLru.removeOne(...)` + `prepend(...)`.
- **Insert-path eviction** — the insert branch pushes the slug to MRU
  front and applies a `while (size > kRoadmapSectionCacheCap)` tail
  eviction.

Behavioural coverage (real cache hits / misses against the live
`cmdRoadmapQuery` path) is already provided by the existing
`mcp_roadmap_section_slice` and `remote_control_roadmap_query` lanes
— those tests rely on the same cache field and continue passing,
verifying byte-identity of cache hits across the LRU refactor.
