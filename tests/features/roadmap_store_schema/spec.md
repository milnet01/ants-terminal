# roadmap_store_schema — schema, location and write-path invariants

Feature contract for **ANTS-3756** INV-6, 7, 8, 10, 11, 14, 17 and 20.
Parent spec: [`docs/specs/ANTS-3756-roadmap-store-schema.md`](../../../docs/specs/ANTS-3756-roadmap-store-schema.md)

## What this locks

**INV-6 — `relates-to` is stored once, normalised on stable identity.** The test
writes the **higher**-sorting endpoint first and asserts the surviving row's
`src` is the *lower* one. Writing the lower first would pass against a writer
that merely rejects the second edge without normalising anything. A second leg
covers the unresolved cross-project case: the local item is `src_pk` whichever
way the pair sorts, because `src_pk` is `NOT NULL REFERENCES item`.

**INV-7 — the store is under `GenericDataLocation + "/ants-terminal"`, never a
cache root.** Asserted on the **resolved path at runtime**, and **neither cache
root may be a prefix of it**. The direction matters:
`~/.cache/ants-terminal/roadmap.sqlite` is not a prefix of the cache root, so a
reversed comparison passes for exactly the placement this forbids.

**INV-8 — a project is keyed on its canonical root.** Three legs: a symlinked
path and the real path are one project; two genuinely distinct roots are two;
and a root that **cannot** be canonicalised is refused, writing no row.
`QFileInfo::canonicalFilePath()` returns an **empty string** for a non-existent
path, so an unchecked writer stores `''` and `root TEXT UNIQUE` then fuses every
missing root into a single shared project — a shadow created by the very call
meant to prevent one. Legs 1 and 2 both pass against that writer; leg 3 is what
catches it.

**INV-10 — provenance is per field, in both directions.** Editing `headline`
sets `provenance.headline` to `asserted` **and** leaves `provenance.kind`
untouched. A one-sided assertion certifies a writer that never updates
provenance at all.

**INV-11 — every closed enum in its own column is rejected at the storage
layer**, not merely documented: `status`, `kind`, `id_origin`, `visibility`,
`element.kind`, `relationship.type`, and `priority` outside 1–5.

**INV-14 — `history` is bounded store-wide and lossless below the bound.** Two
legs needing **two different injected caps**: with a generous cap 60 revisions
are all retained; with a small one the history write fails and reports while the
item write it accompanies still succeeds. One cap cannot serve both — 60
revisions would breach a small one, failing leg (a) against a *correct* writer.

**INV-17 — the store and both WAL sidecars are 0600.** The test performs a
**write** and asserts with the connection **still open**: it is the write that
creates `-wal`/`-shm`, not a checkpoint, and SQLite deletes both when the last
connection closes. A test that checkpoints, or closes before asserting, checks
files that are not there and passes against a store securing nothing.

**INV-20 — every item is filed exactly once.** *At most one* is the partial
index `elem_item_uq`; *at least one* is `putItem()` writing item and element in
one transaction, so a failed element insert rolls the item back.

## Must fail first

Verified by mutating the implementation per each *Breaks when* clause:

- enums written as SQL comments → INV-11's invalid inserts succeed.
- `relates-to` normalised on `item_pk` → INV-6's direction is rowid-dependent.
- `canonicalFilePath()` stored unchecked → INV-8 leg 3 writes `''` twice and the
  second insert collides instead of being refused.
- provenance written per item → INV-10's second half fails.
- a per-item history cap → INV-14 leg (a) evicts.
- `elem_item_uq` dropped → INV-20 leg (b) accepts a second filing.
