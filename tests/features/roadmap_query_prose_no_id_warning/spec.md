# Feature spec: `roadmap_query` prose-roadmap warning parity across status filters (ANTS-3583)

## Problem

On a prose / milestone roadmap with **zero** `[PROJ-NNNN]`-tagged
bullets (e.g. perch's `docs/11-roadmap.md`: `## M1..M9` sections with
`**Status:**` lines and `- **Bold.**` bullets), `roadmap_query` warned
inconsistently:

- `roadmap_query{status:'all'}` → `{count:0, warning:"…dropped all N
  bullet(s)…"}` (ANTS-1538 gate fires).
- `roadmap_query{status:'planned'|'in-progress'|'considered'}` →
  `{count:0}` with **no** `warning`.

Root cause: `preIdPruneCountFull` (the ANTS-1538 warning gate) is
captured **after** the status filter. The status-less narrator bullets
of a prose roadmap don't match a granular status, so the status filter
drops them and zeroes the gate — suppressing the warning. A
status-filtered `count:0` then reads as "no outstanding work" when the
truth is "this tool can't parse this roadmap format" — an agent that
runs a status-filtered query first can wrongly conclude the roadmap is
complete and skip real work (perch feedback 2026-07-18, independently
corroborated same day).

Whether the roadmap has zero id-bearing bullets is a property of the
**file**, not of the status filter, so the signal must be
filter-independent.

## Surface

`RemoteControl::cmdRoadmapQuery` full-file (no `section` arg) emission
path in `src/remotecontrol.cpp`.

## Invariants

- **INV-1 — file-level id-bearing scan on the empty path.** When
  `filtered.isEmpty()`, the handler scans the whole cached bullet set
  (`m_roadmapCacheBullets`) via `shouldDropUnnumbered` to decide whether
  **any** `[PROJ-NNNN]`-tagged bullet exists in the file, independent of
  the status filter. Early-exit on the first id-bearing bullet keeps it
  O(1) on a normal roadmap. Anchor: `ANTS-3583` +
  `fileHasIdBearingBullet` in `src/remotecontrol.cpp`.
- **INV-2 — machine-detectable `parseable_bullets:0` + warning on every
  filter.** When the file has zero id-bearing bullets (and neither
  opt-in is set), the response carries `parseable_bullets` set to 0 and
  a `warning` stating the format is unrecognised, *not* that there is no
  work. This branch precedes the ANTS-1538 / ANTS-3560 branches, so it
  fires identically for `status:'all'` and every granular filter.
  Anchor: `out["parseable_bullets"] = 0` in `src/remotecontrol.cpp`.
- **INV-3 — filter-independence.** The `parseable_bullets:0` branch is
  gated only on the file-level scan + the two opt-in flags
  (`!includeNarratorBullets && !includeSectionHeaders`); it does **not**
  reference the post-status `preIdPruneCountFull`, which is exactly the
  value the status filter was able to zero. A roadmap that *does* have
  id-bearing bullets but whose status filter legitimately matched
  nothing (e.g. `status:'considered'` with zero considered items) skips
  this branch and preserves the prior behaviour (no false format
  warning).

## Test scope

Source-scrape against `src/remotecontrol.cpp` for the anchor strings and
key code patterns. A runtime test would require a RemoteControl +
Roadmap fixture (out of scope here, matching the sibling
`roadmap_query_narrator_filter` / `mcp_roadmap_status_filter` tests).
