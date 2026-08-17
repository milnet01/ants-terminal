# roadmap_write_history — consumer writes leave an audit trail

ANTS-3822. Test contract for the implementation of
`docs/specs/ANTS-3822-consumer-write-history.md`; that spec owns the design and
its own INV numbering, which this file uses unchanged.

## What was wrong

`roadmap_log`'s write ops changed item columns and wrote no `history` row, so a
migrated project's audit trail froze at migration time. `appendHistory()` had
exactly one production caller and it ran during migration.

## Test shape

Behavioural, through the real verbs, on the harness
`tests/features/roadmap_divergence_guard/` established: redirect
`XDG_DATA_HOME` into a `QTemporaryDir`, migrate a small markdown fixture into a
store at `RoadmapStore::defaultPath()`, drive a verb, then read the `history`
table back by direct `SELECT`.

Two rules the harness enforces and this suite inherits:

- **Never default-construct `RoadmapStore`** — `defaultPath()` resolves the
  developer's real store. Every case redirects `XDG_DATA_HOME` first.
- **`Access` is the third constructor parameter**, after the history cap. Which
  is what makes ANTS-3822 § 2.3.2's injectable cap cheap to drive.

Assertions read `history` by raw `SELECT` rather than through a typed reader,
matching `roadmap_store_schema`: the subject is the row that landed, and routing
through a reader would assert the reader instead.

## Invariants under test

Numbering is the spec's.

- **INV-1** — an op that changes an existing item's column writes one row per
  changed column.
- **INV-2** — rows one op writes for one `item_pk` share one `changed_at` and
  carry contiguous `seq`; `seq` is scoped per `(item_pk, changed_at)`, so a
  batch does **not** carry one op-wide counter.
- **INV-3** — `dry_run` leaves the table byte-identical.
- **INV-4** — a rolled-back transaction leaves no rows.
- **INV-5** — at the cap the item write still succeeds, the op returns success,
  and the envelope carries `history_note` with the skipped **row** count.
- **INV-6** — `append` / `append_batch` write no rows.
- **INV-8** — a non-cap `appendHistory()` failure aborts the op.

INV-7 (export round-trip) lives with the export suite, not here.

## Why the red run needs proving first

**Every one of INV-1, INV-2 and INV-6 fails against pre-fix code by returning
zero rows — and zero is also what a broken fixture returns.** A test that
asserts "no rows before, rows after" is green against a fixture that never
migrated anything, never resolved the item, or pointed at the wrong store.

So each case that asserts on rows **first asserts the migration's own rows are
visible to it** (`historyRowCount() > 0` for the fixture's items, which the
migration writes), and only then asserts the consumer's contribution. That
turns "zero" from an ambiguous pass into a real signal.
