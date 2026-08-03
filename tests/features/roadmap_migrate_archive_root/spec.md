# ANTS-3806 — an archive's pre-heading content

Test contract for where a **second source file's preamble** ends up when it
goes through `findRoadmaps()` → `planFrom()` (`src/roadmapmigrate.cpp`) →
`RoadmapMigrateLoad::load()` (`src/roadmapmigrateload.cpp`) →
`RoadmapRender::render()` (`src/roadmaprender.cpp`).

The design contracts are
[`ANTS-3757 § 2.11`](../../../docs/specs/ANTS-3757-roadmap-migration-read.md)
(the synthetic root), [`ANTS-3766 § 2.3`](../../../docs/specs/ANTS-3766-roadmap-migration-archives.md)
(the per-source slug prefix) and
[`ANTS-3758 § 2.8`](../../../docs/specs/ANTS-3758-roadmap-render.md)
(the file preamble). This file does not restate them.

## Why it exists

ANTS-3806 reported that the synthetic root is one row per **project** — empty
slug under `UNIQUE (project_id, slug)` — so every source's pre-heading content
would collapse into one row and an archive's own header would be lost. That was
read off the code and never run.

**Run, it does not happen.** `walkSource()` gives each source's root
`ctx.prefix` as its slug: `""` for the live roadmap, `"<M>-<N>"` for an archive
(`src/roadmapmigrate.cpp`, `planFrom()`). So the roots are distinct rows, the
archive's carries `source_path`, and the render routes it back to its own file.
These two cases are the standing proof, so the claim is not re-derived by
reading again.

## Cases

| Case | Fixture | Asserts |
|---|---|---|
| `ArchivePreambleSurvivesMigrationAndRender` | live `ROADMAP.md` + `docs/roadmap/0.7.md`, each with a marker, its own H1 and one unique prose line | two planned level-0 sections under **different** slugs; both intros stored, the archive's with `source_path = docs/roadmap/0.7.md`; each file renders back its own preamble and **neither acquires the other's** |
| `ArchiveWithNoPreambleStillGetsTheFormatMarker` | live `ROADMAP.md` + `docs/roadmap/0.6.md` opening directly on a `##` heading | no root is planned for that source (`walkSource()` drops a root the source put nothing in), and the render supplies § 3.1's marker itself — **exactly once**, at the head |

The second case is what keeps ANTS-3758's constant marker earning its place:
that fallback is for a file with **no root section**, which is a file with no
pre-heading content — not, as § 2.8 first said, every archive.

## Must-fail-first

Both were shown red against their own defect before the source was restored:

| Case | Injected defect | Observed |
|---|---|---|
| 1 | `root.slug = ctx.prefix` → `QString()` in `walkSource()` (the shape ANTS-3806 described) | red on the distinct-slug assertion; and `load()` **refuses** with `archive_slug_collision`, so even the reported shape could not have lost data silently |
| 2 | the marker fallback `if (!sawRoot \|\| !hasMarkerInHead(text))` → `if (false)` | red on the marker assertions, while case 1 stayed green — its marker comes from the replayed intro, so the two paths are discriminated |

## Out of scope

**Bullet-body fidelity.** Rendering here duplicates each item's headline and
fields, because the migration stores `BulletRecord::body` (the full bullet)
into `item.body` while the render reads that column as residual prose. Found by
this fixture, tracked as **ANTS-3808**, and asserted there rather than here —
these cases are about the preamble.
