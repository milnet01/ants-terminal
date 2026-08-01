# roadmap_store_schema — schema, location and write-path invariants

Feature contract for **ANTS-3756** INV-6, 7, 8, 10, 11, 14, 17, 20, 21 and
22–25. Parent spec: [`docs/specs/ANTS-3756-roadmap-store-schema.md`](../../../docs/specs/ANTS-3756-roadmap-store-schema.md)

INV-22–25 arrive with **ANTS-3765**, whose own numbering calls them INV-7, 8, 9
and 10. This directory already tests ANTS-3756's INV-7, INV-8 and INV-10 — all
three of which mean something else — so they are filed here under ANTS-3756's
next free numbers, and the test names use those. Spec:
[`docs/specs/ANTS-3765-roadmap-migration-load.md`](../../../docs/specs/ANTS-3765-roadmap-migration-load.md)

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

**INV-21 — `lanes`, `evidence` and `extras` are reachable, and stored
canonical.** Two legs, because the two writers fail independently: `putItem()`
had no field for any of the three and `setItemField()`'s allowlist excluded
them, so both had to be widened. Read back through **raw SQL**, never through a
getter — the defect is that the columns held their DDL defaults while every call
reported success, which a round-trip through the writer's own idea of the value
cannot see. Leg (a) puts `extras.tiny = 0.000001`, the ECMAScript
fixed-versus-exponential boundary: JCS writes `0.000001`, Qt's
`toJson(Compact)` writes `1e-06`, so that one value is what makes *canonical*
assertable rather than merely *written*. Leg (b) passes deliberately
out-of-order keys and asserts the stored bytes are sorted, asserts provenance is
still per field across a JSON-column write (INV-10), and refuses three values
that **parse** as JSON and are the wrong shape for their column — a
parse-only guard accepts all three.

**INV-20 — every item is filed exactly once.** *At most one* is the partial
index `elem_item_uq`; *at least one* is `putItem()` writing item and element in
one transaction, so a failed element insert rolls the item back.

**INV-22 (ANTS-3765 INV-7) — `begin()` refuses to nest.** A second `begin()`
fails, reports, and leaves the open transaction alone; the first one's `commit()`
still commits its writes. `commit()` and `rollback()` with none open refuse
rather than returning success — a silent no-op there is the same defect one step
later, where the caller believes it has ended a transaction and has not.

**INV-23 (ANTS-3765 INV-8) — `putItem()` is atomic either way, and never rolls
back a transaction it does not own.** Three legs, and the third is the one that
matters: a **failing** `putItem()` inside an open transaction must leave
`inTransaction()` true. Legs (a) and (b) — self-commit standalone, and no row
surviving a caller's rollback — both pass against a `putItem()` that issues its
old internal `ROLLBACK`, after which the caller carries on believing it is in a
transaction while every later write autocommits and persists, and the migration
reports a clean partial load.

**INV-24 (ANTS-3765 INV-9) — two of the four new writers canonicalise, and two
store prose verbatim.** `setLegend()` and `addElement(kind='table')` produce
sorted, compact bytes from deliberately out-of-order input;
`addElement(kind='narration')` and `setSectionIntro()` round-trip prose byte for
byte **including prose that looks like JSON**, which ANTS-3756 § 2.3 calls
undefined to canonicalise rather than merely wasteful.

**INV-25 (ANTS-3765 INV-10) — the filing paths are closed.** `addElement()`
refuses `kind='item'` and `fileItem()` refuses an already-filed item, so
`putItem()` and `fileItem()` stay the only ways an item acquires a filing. A
third leg covers `unfileItem()`, the only way back: it must take **one item's**
filing and leave the section's other element rows alone, since a narration row
in that section is payload nothing re-inserts. The
assertion is on the refusal's **shape**, not its existence, and that is forced
by the schema: the `element` CHECK and `elem_item_uq` refuse both cases too, so
a method that passed its arguments straight through would also return `false`.
What distinguishes them is which layer spoke — a reported misuse, or a
constraint violation the caller must now parse — so the error text is where the
invariant is observable.

## Must fail first

Verified by mutating the implementation per each *Breaks when* clause:

- enums written as SQL comments → INV-11's invalid inserts succeed.
- `relates-to` normalised on `item_pk` → INV-6's direction is rowid-dependent.
- `canonicalFilePath()` stored unchecked → INV-8 leg 3 writes `''` twice and the
  second insert collides instead of being refused.
- provenance written per item → INV-10's second half fails.
- a per-item history cap → INV-14 leg (a) evicts.
- `elem_item_uq` dropped → INV-20 leg (b) accepts a second filing.
- the three JSON columns left out of `putItem()`'s INSERT and out of
  `setItemField()`'s allowlist — the shipped state ANTS-3767 fixed → INV-21
  leg (a) reads back `[]`/`{}` and leg (b) refuses `field not writable`.
- either writer binding `QJsonDocument::toJson(Compact)` instead of
  `canonicalJson()` → leg (a)'s `tiny` reads `1e-06`.
- a JSON-column write validated by parse alone → leg (b)'s three wrong-shape
  values are accepted.
- `begin()` no-oping inside an open transaction → INV-22's second `begin()`
  returns true and reports nothing.
- `putItem()`'s `ROLLBACK` issued on its failure paths whoever owns the
  transaction — the shipped shape — → INV-23's caller-side `rollback()` fails
  with "no transaction is active", because `putItem()` already ended it.
- `setLegend()` binding `QJsonDocument(o).toJson()` → INV-24 reads back indented
  JSON; `addElement()` canonicalising narration instead of `table` → the prose
  payload is refused as "not JSON" and the table payload keeps its input order.
- `addElement()` passing `kind` through, and `fileItem()` leaning on
  `elem_item_uq` → INV-25 gets `CHECK constraint failed` and `UNIQUE constraint
  failed: element.item_pk` in place of the two API refusals.
- `unfileItem()` deleting by the item's **section** rather than by the item →
  INV-25's third leg finds the section's narration row gone with it.
