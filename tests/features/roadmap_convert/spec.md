# ANTS-4491 — convert a github-task-list roadmap to canonical ants-v1

Contract for `tests/features/roadmap_convert/test_convert.cpp`.
Design: [`docs/specs/ANTS-4491-dialect-convert.md`](../../../docs/specs/ANTS-4491-dialect-convert.md).

## Problem

A project still on the `github-task-list` dialect cannot adopt the standard
format without rewriting its whole roadmap by hand, allocating an id for every
bullet that lacks one. The blocked consumer holds a standing decision not to do
that by hand, so the conversion belongs in the tool.

Neither single-step order is safe. `RoadmapSource::migratedProject()` dispatches
on the live file's detected format and refuses a disagreement with the stored
`source_format`, so rewriting the file first and setting the column first each
leave the pair disagreeing — and every read then refuses.

## Fix

`roadmap_log op:"convert"` rides `RoadmapWrite::commitAndRender()`. Its
`mutate()` re-imports the file, allocates ids for the bullets that lack one, and
sets `source_format` to `ants-v1`. `commitAndRender()` reads the dialect from the
store *after* `mutate()` runs, so the validating dry render and the publish both
emit the new dialect inside one sequence.

The load borrows that transaction rather than opening its own —
`RoadmapStore::begin()` refuses to nest, and INV-4 requires the allocation and
the commit to be one transaction.

## Invariants

- **INV-1** — after a successful convert the file's detected format and the
  stored `source_format` agree. *Test:* `formatsAgree`.
- **INV-2** — a convert that fails before commit leaves the store and the file
  exactly as they were. *Test:* `failedConvertIsInert`.
- **INV-3** — converting twice produces the same file as converting once.
  *Test:* `convertIsIdempotent`.
- **INV-4** — no id changes between the allocation that assigns it and the
  commit that persists it. *Test:* `idsStableAcrossCommit`.
- **INV-5** — a bullet already carrying a bracket id keeps that id.
  *Test:* `existingIdsPreserved`.
- **INV-6** — an id present in the file but absent from the store matches its
  existing item rather than creating a second one.
  *Test:* `staleMirrorDoesNotDuplicate`.
- **INV-7** — a source recognised as a third dialect refuses
  `dialect_out_of_scope` and writes nothing. *Test:* `thirdDialectRefused`.

## Notes

The forced failure INV-2 and INV-4 use is the render's Layman gate: an open item
with no `Layman:` line refuses the whole write after `mutate()` has run and
before the commit, which is exactly the window those two are about. So every
other fixture gives each open bullet a `Layman:` line.

INV-7's fixture carries two `Pass` headings and two `Status` markers and no
emoji bullets. With fewer, `detectRoadmapFormat()` classifies it `ants-v1` and
the case exercises the accepted path instead.

Label: `features`. Bundle: `test_claude`.
