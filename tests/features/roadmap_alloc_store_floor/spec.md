# ANTS-4493 — the markdown allocator floors to the store's high-water

## Problem

`roadmap_log` allocates through two paths, and each floors to two of the
three places an id can already exist. Both `op:"append"` and
`op:"append_batch"` reach them.

- The **store path** (`ANTS-3809`) uses `rlStoreIdHighWater()` —
  `max(corpusHighWater, idHighWater)`.
- The **markdown path** uses `max(rlMaxExistingIdForPrefix(ROADMAP.md),
  corpusHighWater)` and never reads the store.

A project that has been migrated but is **not served from the store** takes
the markdown path: `roadmapWriteTarget()` resolves through
`RoadmapSource::migratedProject()`, which returns `nullopt` for every
dialect but `ants-v1` (`src/roadmapsource.cpp`, "legitimately
markdown-served"). Its synthesised ids live in the store and in no file, so
the file+corpus floor cannot see them.

Reported against Vestige (`github-task-list`, 989 GFM bullets beside 36
ants-v1 ones): the file's max was `3D_E-0611`, the allocator issued `0612`
— and the store already held `3D_E-0612`, synthesised onto an unrelated
bullet. Two items, one id, in the two stores that are supposed to become
one. The `id` column is the locate key for flip / annotate / amend, so a
duplicate is not cosmetic.

It fires **once per migrated project, at the first append**, silently,
with `ok:true` and a normal-looking id.

## Fix

In the markdown path, floor `maxFileId` to the store's high-water for the
resolved prefix as well. The project is looked up by
`readProjectByRoot()` — not `migratedProject()` — because the question is
"does the store hold ids for this root", which is true whatever dialect the
file is in.

## Invariants

- **INV-1** — An append to a migrated project whose file is behind the
  store does not reissue a stored id. *Test:* migrate a mixed
  `github-task-list` fixture whose only declared id is `DEMO-0001` and
  whose two id-less bullets are synthesised as `DEMO-0002` / `DEMO-0003`;
  then append and assert the new id is `DEMO-0004`. *Breaks when:* the
  allocator reads the file and the corpus only — it then issues
  `DEMO-0002`, which is the reported collision.
- **INV-2** — The floor is additive, not a replacement: a project with no
  store row allocates exactly as before. *Test:* the same fixture with no
  migration run first allocates `DEMO-0002`.
- **INV-4** — `op:"append_batch"` floors to the store as `op:"append"`
  does. The fix landed in the scalar op alone, so the batch op reissued a
  synthesised id — the same collision, one verb over. *Test:* INV-1's
  fixture driven through the batch op; its single returned id is
  `DEMO-0004`. *Breaks when:* the batch path reads the file and the corpus
  only, and issues `DEMO-0002`.
- **INV-5** — `op:"append_batch"` resolves `.roadmap-counter` beside the
  RESOLVED roadmap, not under `caller_cwd` (ANTS-3350, which also landed
  in the scalar op alone). The counter's directory is what the
  committed-corpus floor is derived from, so a wrong one silently disables
  that floor too. *Test:* call from a subdirectory of a project whose
  roadmap sits at the root; no counter appears in the subdirectory and the
  one beside the roadmap is what advanced. *Breaks when:* the op resolves
  under `caller_cwd` — it then refuses `counter_missing`, naming a path in
  the subdirectory. The fixture needs a `.git`: without a repo boundary
  the ancestor walk returns the caller's own directory and the subdirectory
  case cannot arise.
- **INV-6** — The store floor applies to a caller in a project
  SUBDIRECTORY, in both allocating ops. `readProjectByRoot()` is keyed on
  the canonical project ROOT, so asking it by `caller_cwd` matches nothing
  from a subdirectory and the floor silently does not apply — reachable
  since ANTS-3350 let a write verb resolve the roadmap from one. The key is
  the directory `findRoadmapUnder()` matched the roadmap UNDER, not the
  roadmap file's own directory: those coincide only while the roadmap sits
  at the root, and that helper also resolves `docs/` and `.github/`.
  *Test:* INV-1's and INV-4's fixtures called from a `sub/` directory of a
  project carrying a `.git`; each new id is `DEMO-0004`. *Breaks when:* the
  floor is looked up by `caller_cwd` — both ops then issue `DEMO-0002`,
  which is INV-1's reported collision.
