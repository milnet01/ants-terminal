# Repair the truncated trailer columns — ANTS-4585 phase 2

**Status:** implemented (2026-08-20)

## Problem

Migration parsed each bullet's trailer with the parser of the day, and
that parser cut a value short in two ways: at a hard line wrap
(ANTS-4542) and at the full stop inside `e.g.` / `etc.` / `config.yaml`
(ANTS-4596 for `layman`, ANTS-4597 for `lanes`). The causes are fixed;
the short values are still in every migrated store.

They are repairable because the legacy inline run was never stripped
from the body, so the author's full text survives in the item's own
prose. Measured at HEAD over 5076 items in 16 projects: re-parsing that
prose with the CURRENT parser recovers 824 `layman` values (+82,362
characters), 58 `source` (+2,323) and 54 `lanes`.

**The danger is not the truncation, it is the repair.** Of the values
where a re-parse disagrees with the column, eleven are not recoveries.
Seven are post-migration hand edits — the stored value is NEWER than
the prose, because `roadmap_log` updated the column and left the legacy
run alone. ANTS-1933, ANTS-1934, ANTS-1931, ANTS-1932, ANTS-1884,
ANTS-1565 and ANTS-1579 all read that way. An unguarded re-parse
reverts each to superseded text, and there is no second copy to check
against afterwards.

## Contract

**The repair is a re-parse, not a bespoke un-truncation.** The value
written is what `RoadmapParse::parseAntsV1Bullet()` reads from the
item's own stored body today. Nothing reconstructs a cut boundary.

**The bullet is reconstructed from the head line and the stored body
ONLY — never from the render.** `RoadmapRender::bulletText()` composes
a trailer line for every column the prose does not declare (ANTS-4599),
so re-parsing its output hands the stored column straight back and the
repair is a no-op by construction.

**A value is written only when the stored one is a strict prefix of the
re-parsed one.** Equal values are left alone. Anything else — a
divergence, a shortening, a reordering — is SKIPPED and reported, never
written. This is the whole of the protection described above.

**`lanes` compares element-wise.** The stored list must be a leading
sublist of the re-parsed list. Comparing a joined string diverges on
separator spacing alone.

**Provenance stays `asserted`.** The recovered text is the author's own
adjacent prose and the guard only ever extends an existing asserted
value, so the repair recovers an assertion rather than inventing one.
`store-generated` would reclassify an author field as store-populated.

**`dry_run` decides every write before opening a transaction**, so the
preview reports the real run's counts rather than a second estimate.

**What this does NOT do:** repair an item whose prose no longer carries
the run. The cut text is gone and no re-parse recovers it; those items
are invisible to this pass and are not counted as repaired.

## Invariants

- **INV-1** — a value truncated at an internal full stop is repaired
  from the surviving prose, and the recovered value is the whole run.
- **INV-2** — a value truncated at a hard line wrap is repaired, the
  wrapped remainder joined into one line.
- **INV-3** — a stored value that is NOT a prefix of the re-parse is
  left exactly as it was, and is counted as skipped. The guard fires on
  a shortening and on a divergence alike.
- **INV-4** — an already-correct value is not rewritten, and does not
  count as a repair.
- **INV-5** — `dry_run` writes nothing and reports the same counts the
  real run then performs.
- **INV-6** — `lanes` gains whole members; a stored list that is not a
  leading sublist of the re-parse is skipped.
- **INV-7** — the pass is idempotent: a second run over a repaired
  store reports zero repairs.
- **INV-8** — an item whose prose carries no trailer run is untouched.
