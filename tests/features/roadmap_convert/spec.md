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

- **INV-8** — the render's Layman gate is ADVISORY for `op:"convert"`: an open
  item with no `Layman:` line does not refuse the conversion, and the envelope
  reports what it let through in `layman_missing`. The exemption ends with the
  convert — an ordinary write touching the same item is still refused.
  *Test:* `laymanGateIsAdvisoryForConvert`.
  *Why:* a convert is a migration, not authoring. No new claim enters the
  project; the same bullets change representation. It also cannot satisfy the
  gate in principle — `Layman:` is an ants-v1 rule, and the source dialect is
  one where roadmap-format.md makes it optional, so enforcing it demands the
  DESTINATION dialect's rule of items that exist only in the source dialect, as
  a precondition of the call that would make that rule apply. Measured on
  Vestige 2026-09-21: 460 open items, so the op refused outright on the one
  project it was built for.
  *Breaks when:* `commitAndRender` is called without `LaymanGate::Exempt`, or
  the exemption leaks past the convert to ordinary writes.

- **INV-9** — the envelope reports the id assignment PER BULLET, one row per
  bullet, each carrying `origin`, `in_file` and the source `line`.
  *Test:* `dryRunReportsIdOriginPerBullet`, `reportDistinguishesExistingIdsFromAllocated`.
  *Why:* the op is a one-way bulk rewrite of a version-controlled public file
  that moves a counter other documents cite by id. An aggregate count cannot be
  checked against anything; a per-bullet list can be read against the file. The
  asymmetry is what makes `in_file` the column to scan — a newly assigned id is
  visible and fixable, a CHANGED one is invisible and permanent.
  *Breaks when:* the report samples rather than covers, or `in_file` is
  computed from the store instead of from what the file holds.

- **INV-10** — `origin: "absent"` claims only that the FILE carries no id for
  that bullet. It does not claim an allocation happened: the load may instead
  match the bullet to an existing store item by headline (INV-6).
  `ids.allocated_ids[]` names the ids actually issued.
  *Test:* covered by `reportDistinguishesExistingIdsFromAllocated`, which
  asserts the parsed and absent populations are distinguishable.
  *Why:* the row is built from the plan, BEFORE the load resolves it, so a
  label of "allocated" would assert something the row cannot know — on the one
  field a reviewer is trusting most.
  *Breaks when:* the value is renamed to something that asserts an allocation.

- **INV-11** — a bullet the load MATCHES to an existing store row reports
  `matched: true` and names `matched_id` and `matched_headline`. A summary
  `ids.matched` counts them over every row, not only the capped echo.
  *Test:* `reportNamesMatchedRows`.
  *Why:* this is the arm worth previewing, and the reason inverts the obvious
  reading. A freshly allocated id collides with nothing; a match writes an
  EXISTING id into the file, and if it is the wrong row every prior citation of
  that id resolves to the wrong work. The output is well-formed either way, so
  it cannot be reviewed afterwards.
  *Breaks when:* the load's per-item outcome stops reaching the report, or
  `matched:true` is emitted without naming the id.

- **INV-12** — where several stored rows satisfy the match key, § 2.6.1 pairs
  them BY ORDER and the report carries `ambiguous_rematch` on each affected
  ROW, plus a count.
  *Test:* `ambiguousRematchIsReportedPerBullet`.
  *Why:* the pairing is reproducible but rests on order alone, which the code
  says of itself. An `ambiguous_rematch` NOTE has always fired; a load note
  carries no line and cannot be correlated back to a bullet, so a caller could
  learn that something was paired by order and never which. That is the one arm
  a human should check and it was the least reachable.
  *Breaks when:* the flag is emitted only as a note, or only in the summary.

- **INV-13** — a convert preserves PROSE, not only bullets: narration above
  the first heading, a section intro between a heading and its first bullet,
  and a tail note after the last bullet all survive the rewrite.
  *Test:* `convertPreservesNarration`.
  *Why:* the preamble is where a roadmap explains how to file into it. A
  convert that keeps bullets and drops narration produces a well-formed file
  that has lost its own instructions, and nothing in the id report would say
  so. Three placements rather than one because different machinery carries
  each — keeping one and dropping another would pass a laxer test.
  *Breaks when:* an element kind stops being carried, or the synthetic
  empty-slug section that holds the preamble is dropped.
  *Limit, stated:* the fixture is one bullet and three paragraphs. It proves
  the mechanism carries narration in each placement; it does not prove a large
  real roadmap survives intact, and no fixture here can.

Note on a field that is deliberately absent: the report does NOT carry the
matched row's id origin. Both match passes require `idFromMigration` on the
candidate, so every matched row is migration-allocated by construction — a
field for it could hold only one value, and a constant dressed as data invites
a reader to believe it was checked.

## Notes

The forced failure INV-2 and INV-4 use is `RoadmapWrite::setForcePostMutateFail
ForTest()`, a seam that aborts in the window after `mutate()` has run and before
the store commits — which is the property both invariants are about.

It was the render's Layman gate until ANTS-5256, which exempted this op from
that gate. Both cases would then have gone green by LOSING their trigger rather
than by holding, leaving the two invariants that guard the irreversible half of
this op unguarded by a passing suite. A test that forces its failure through
whichever business rule is handy is coupled to a rule that was never its
subject; a seam named for the window cannot be invalidated by a rule change.

INV-7's fixture carries two `Pass` headings and two `Status` markers and no
emoji bullets. With fewer, `detectRoadmapFormat()` classifies it `ants-v1` and
the case exercises the accepted path instead.

Label: `features`. Bundle: `test_claude`.
