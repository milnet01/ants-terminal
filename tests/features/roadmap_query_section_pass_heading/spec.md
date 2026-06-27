# Feature: roadmap_query section= surfaces nested `#### Pass N.M` bullets (ANTS-2225)

## Problem

`roadmap_query` with a `section=<slug>` filter slices that one section's
markdown and runs `RoadmapDialog::parseBullets` on the **slice**
(`remotecontrol.cpp` section= cache-miss branch). The pass-headings adapter
(`detectRoadmapFormat`) only engages when it sees **≥ 2** `#### Pass N.M`
headings AND **≥ 2** `- **Status**:` markers across its input. A single
section often carries just **one** pass heading, so slice-local detection
fails, `parseBullets(slice)` returns zero, and the section is reported
`count: 0` / `section_shape: "prose"` — even though the same bullet is
surfaced by `session_orient` active_bullets, an `id=` query, and
`mode: section_index` (all of which parse the **whole** file, where the 2+2
threshold is met). The `section_index` count (1) and the section= fetch (0)
disagree for the same slug — RetroDB Pass 49.1.

## Contract

- **INV-1 whole-file parse surfaces it** — `parseBullets(wholeDoc)` on a
  pass-headings roadmap yields the synthesised `PASS-<major>-<minor>` bullet
  and tags it with its section's global slug.
- **INV-2 slice-local detection fails** — `parseBullets(sectionSlice)` for a
  section that holds exactly one `#### Pass N.M` heading returns zero bullets
  (the 2+2 threshold is unmet in isolation). This is the reproduced gap.
- **INV-3 whole-file filter recovers it** — filtering the whole-file parse by
  `sectionSlug == <slug>` yields exactly the section's pass bullet. This is the
  mechanism the section= fallback uses: when the slice parses empty, re-parse
  the whole doc and keep the bullets whose global slug matches.
- **INV-4 fallback wired** — the `roadmap_query` section= cache-miss branch in
  `remotecontrol.cpp`, on an empty slice parse, re-parses the whole markdown
  and filters by `sec->slug` before classifying the section shape (so a
  recovered bullet suppresses the now-wrong prose hint).

## Out of scope

- A non-pass-heading doc still yields no bullet for a prose-only section
  (the fallback filters the whole-file parse, which finds nothing there).
- The MCP schema is unchanged (a default-behaviour fix, no new arg).
