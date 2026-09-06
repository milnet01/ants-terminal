# ANTS-4884 — the store dispatch resolves the project from a subdirectory

## Problem

`RemoteControl::roadmapBullets()` and `RemoteControl::roadmapWriteTarget()`
are the read-side and write-side dispatchers: each asks
`RoadmapSource::migratedProject()` whether the store holds this project,
and takes the markdown path when it does not.

Both are handed `caller_cwd`. `migratedProject()` asks
`readProjectByRoot()`, which keys a project on its canonical ROOT — so from
a project subdirectory the lookup matches no row, the dispatcher reads
"not migrated", and the verb takes the markdown path. Reachable since
ANTS-3350 let a verb resolve the roadmap from a subdirectory; before that a
subdirectory caller was refused outright.

Measured on Ants Terminal, which is served from the store: `roadmap_query`
reports `source:"store"` from the root and `source:"markdown"` from `src/`.

It is worse on the write side. `ROADMAP.md` is an OUTPUT of the store, so a
markdown splice from a subdirectory writes an item the store never sees,
and the next store-path write re-renders the file over it. The append
reported `ok:true`.

This is the silent fallback `RoadmapSource`'s INV-1 forbids, arriving
through a door the invariant does not watch — its own comment says as much
about a non-canonical path, and a subdirectory path is the same class.

## Fix

Both dispatchers resolve the project root before asking: the directory
`findRoadmapUnder()` matched the roadmap under (ANTS-4882 added that
report), which is the root the store was registered with. A path with no
roadmap above it resolves to itself, so a caller outside any project is
unchanged.

`roadmapBullets()` also passed `caller_cwd` to
`ProjectSettings::idFormatFor()`, which reads `.ants/project.json` at the
root; that misses from a subdirectory for the same reason and is resolved
by the same line.

## Invariants

- **INV-1** — A read verb serves a migrated project from the store when
  called from a subdirectory. *Test:* migrate an ants-v1 fixture, then
  `roadmap_query` from `sub/`; `source` is `store`. The subject and the
  root-call control need SEPARATE `RemoteControl` instances: `source` is
  served from a per-instance cache keyed on the roadmap's path and mtime,
  not on `caller_cwd`, so a root call first fills it with `store` and the
  subdirectory call reads that back while the defect is live. *Breaks
  when:* the dispatcher asks by `caller_cwd` — it answers `markdown`,
  having read a file that is an output of the store it declined to open.
- **INV-2** — A write verb allocates and writes through the store when
  called from a subdirectory. *Test:* `op:"append"` from `sub/`; the
  envelope carries no `line`, which only the markdown splice reports.
  *Breaks when:* the dispatcher asks by `caller_cwd` — the append splices
  the markdown file instead, and the next store render discards it.
- **INV-3** — A caller outside any project is unchanged. *Test:* a
  directory with no roadmap at or above it resolves to itself and the read
  reports `no_roadmap` as before. *Breaks when:* the resolution walks
  somewhere and reports another project's answer.
