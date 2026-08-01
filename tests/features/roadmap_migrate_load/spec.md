# roadmap_migrate_load — the migration load half

Feature contract for **ANTS-3765** INV-1, 2, 3, 4, 5, 6, 11, 12, 13, 14 and 15.
Parent spec: [`docs/specs/ANTS-3765-roadmap-migration-load.md`](../../../docs/specs/ANTS-3765-roadmap-migration-load.md)

That spec's other four invariants — INV-7, 8, 9 and 10 — constrain **store
methods**, not loader behaviour, and are filed with the rest of ANTS-3756's
write-path invariants in
[`roadmap_store_schema`](../roadmap_store_schema/spec.md), where they are
numbered INV-22–25.

**The plans are constructed, never parsed.** ANTS-3757 already owns the
markdown; a fixture-driven test here would exercise its parser again and could
not build the one input INV-1 needs — an off-enum `status`, which the read half's
status vocabulary is total over and therefore cannot emit.

## What this locks

**INV-1 / INV-11 — one project is one transaction, and the project row exists
exactly when the plan committed.** A plan whose *second* of three items violates
a CHECK leaves **no row in any table**: not the project, not the sections, not
the first item, not `id_prefix`. The first item is the point — the shipped
self-committing `putItem()` would have left it behind, along with everything
written before it. Byte-identity of the file is deliberately not asserted: WAL
frames and freelist pages move under a rolled-back transaction.

**INV-2 — a re-run over unchanged source changes no item and writes no
history.** Two legs, and the second is the one that matters. Leg (a) is a plan
whose items all carry ids. Leg (b) carries **none**, which is ~40% of the real
corpus: id-less items cannot be matched by id, and § 2.8 allocates their ids
inside the store while § 5 forbids writing them back to source, so the second
run sees the same bullets id-less again. Matched by id alone every one of them
is re-inserted with a fresh id and its predecessor orphaned — the corpus
duplicated on every pass. Leg (a) passes against a loader with no id-less rule
at all, which is why it cannot be the only leg.

**INV-3 — a re-run never clears a field the plan does not carry.** `milestone`
and not `priority`: `priority` is in neither `setItemField()`'s allowlist nor
`QString`-typed, so the obvious recipe cannot run at all.

**INV-4 — an item absent from source is retained, re-filed and reported.** The
recipe has an **intervening run** that edits the second item's headline, because
an initial load writes no history — without it the test asserts the survival of
rows that were never created. After the omitting run the orphan keeps its row,
its history row and its status, both items still satisfy ANTS-3756 INV-20
(exactly one `element` row each), and an `orphaned_item` note names it.

**INV-5 — ordering is rebuilt, not shifted.** A three-item section re-loaded
**reversed** must succeed, and afterwards the positions are exactly `0..n-1`.
`element` carries `UNIQUE (section_id, position)` and SQLite enforces it per row
as each is written, so an in-place shift fails at the first row whose new
position is still held by one that has not moved. The order assertion puts its
`ORDER BY` **inside a subquery**: a bare `ORDER BY` applies after aggregation
and would not order what `group_concat()` concatenates.

**INV-6 — a rolled-back load allocates no id.** Two legs, each stating its
expected value, because "asserts the high-water" is satisfiable by a test that
asserts nothing: on a first run `id_prefix` holds **no row**; on a re-run seeded
to 41 it still holds **41**.

**INV-12 — a load against an `Interactive` store is refused**, rather than run
against a 5 s deadline that fails only sometimes — which would pass locally and
fail on a loaded machine.

**INV-13 — `dryRun` writes nothing and reports what a real run would have
done.** Compared count by count **excluding `projectId`**, which is a rowid the
dry run rolled back, so any equality there would pass or fail on SQLite's rowid
reuse. The plan carries an id-less item and a narration element so the dry run
has allocation and element writes to report.

**INV-14 — a re-run's `history` rows never collide.** Three loads sharing **one**
`changedAt`, two of them changing the headline, produce `seq` 0 and 1 rather
than a `UNIQUE (item_pk, changed_at, seq)` violation. The trigger is a caller
stamping two runs identically, which is not a misuse.

**INV-15 — a history write refused at the cap does not abort the project.** The
only exception to INV-1, asserted rather than merely written down: with an
injected 8-byte cap the item's new headline still commits, `ok` is `true`, and a
`history_capped` note carries the loss.

## Must fail first

Every invariant was shown RED against the mutation its *Breaks when* clause
names, in two rounds, before the implementation was restored:

- `load()` opening no transaction — the shipped `putItem()` shape → INV-1/11:
  the failed load leaves the project, its section and item A-1 behind.
- the field comparison rewriting every field on every run → INV-2 leg (a):
  `itemsUpdated == 2` and two `history` rows on a run that changed nothing.
- § 2.6.1's id-less natural key removed → INV-2 leg (b): `itemsInserted == 2` on
  the second run, two ids burnt, the first run's rows orphaned.
- `milestone` added to the plan's authoritative set with the "empty does not
  overwrite" exception widened to every field → INV-3: the human edit is gone.
- the rebuild re-filing only the plan's items → INV-4: the orphan ends the
  transaction unfiled and ANTS-3756 INV-20 is false at a commit boundary.
- positions updated in place with `UPDATE element SET position` instead of the
  per-section clear → INV-5: `UNIQUE (section_id, position)` fires on the
  reversed order.
- allocation run before the transaction → INV-6: leg 1 finds an `id_prefix` row
  after a failed load, leg 2 finds 44 where 41 was seeded.
- the `Access` check removed → INV-12 loads happily into an Interactive store.
- the dry run short-circuiting before the writes → INV-13: every count is 0
  against the real run's.
- `seq` restarting at 0 per run → INV-14: the third load aborts on the UNIQUE.
- every `appendHistory()` failure treated alike → INV-15: the whole project is
  rolled back because its audit trail is full.
