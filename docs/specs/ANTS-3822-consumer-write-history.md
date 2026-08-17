# ANTS-3822 — consumer writes append a history row

**Status:** spec draft (2026-08-17).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3822 (in-session-2026-08-04, ANTS-3809 cold-eyes loop 1 lane A; picked up 2026-08-17 on user request after ANTS-4414 measured what its absence costs).
**Blocked by:** none — ANTS-3809 shipped, which is the write path this hooks into.
**Blocker for:** the store-backed last-touch reader that would retire ANTS-4414's `git blame`.

## 1. Problem

`RoadmapStore` has a `history` table and an `appendHistory()` writer, and
**`roadmap_log`'s eight write ops populate neither.** They change the item
column and move on, so the audit trail has a hole that opens the moment a
project migrates.

Eight is `ANTS-3809` § 2.2's table, counted rather than recalled:

```
$ awk '/^### 2.2 The eight ops/,/^### 2.3/' \
    docs/specs/ANTS-3809-roadmap-write-half.md | grep -cE '^\| `[a-z_]+` \|'
8
```

Measured 2026-08-17:

```
$ rg -n 'appendHistory\(' src/ --glob '*.cpp' \
    | grep -v '^\S*: *//' | grep -v 'RoadmapStore::appendHistory'
src/roadmapmigrateload.cpp:505:    if (store.appendHistory(itemPk, opts.changedAt, *seq, …
```

**One production call site.** `Loader::recordHistory()`
(`src/roadmapmigrateload.cpp`) is the only thing in the tree that calls it,
and it runs during migration. There is a second *producer* of `history` rows —
the export's rebuild inserts them directly rather than through the writer
(`src/roadmapexport.cpp`, its own comment says why) — but that path replays an
export, so it creates no history that did not already exist.

The consequence is that **a migrated project's history is frozen at migration
time.** Every `flip`, `annotate`, `amend_body` and `append_batch` since is
invisible. A field's previous value is recoverable for a migrated write and
not for a consumer one, which inverts the usual expectation that recent
changes are the best-recorded.

**This is not hypothetical, and ANTS-4414 measured the bill.** The roadmap
dialog wants one fact — when was this item last touched — for its in-progress
cards. The store cannot answer it, because the only rows that could are older
than every edit the user has made since migrating. So the dialog asks
`git blame`, which on this project's `ROADMAP.md` takes **3.71 s** and was 98%
of the dialog's open time. ANTS-4414 moved that off the GUI thread; it could
not remove it, because the data genuinely is not in the store. **Filling this
hole is what makes the blame deletable.**

**Layman:** The roadmap database has a table for "who changed what, when", and
the tools that change things do not write to it. So the database knows the
history up to the day the project moved into it, and nothing since.

## 2. Surface

### 2.1 What a revision is — one row per changed field, grouped by stamp

**The shape is already decided; this spec adopts it rather than choosing.**
`Loader::applyPlanFields()` writes one `history` row **per changed column**,
all sharing one `changed_at`, with `seq` incrementing across them. The table's
`UNIQUE (item_pk, changed_at, seq)` is what makes that addressable.

So a revision is **the `(item_pk, changed_at)` group**, and its member rows are
its changed fields. An op that writes a body plus five re-derived trailer
columns produces **six rows, one revision** — not six revisions and not one
row summarising six changes.

Adopting the migration's shape is the point. A second convention would mean
`history` rows whose meaning depends on which writer produced them, and the
export round-trips every row through one format regardless.

**Two consequences worth stating because they are not obvious.**

`changed_at` is second-resolution (the DDL's `CHECK … GLOB` pins
`YYYY-MM-DDThh:mm:ssZ`). **Two ops touching the same item within one second
therefore merge into one revision** — same stamp, contiguous `seq`. Nothing
downstream distinguishes them, and for the last-touch reader this spec exists
to unblock, nothing needs to.

`seq` continues from `maxHistorySeq(itemPk, changedAt)` and never restarts at
0 for a stamp that already has rows. The migration's comment gives the reason
and it applies unchanged here: a restart collides on the UNIQUE constraint and
aborts the whole write.

### 2.2 Where it hooks — inside the existing transaction, not beside it

Every op already runs inside one store transaction, opened by
`RoadmapWrite::commitAndRender()` (`src/roadmapwrite.cpp`): `store.begin()`,
then the op's `mutate()` callback, then a dry render and gate check, then
either commit or `abort()`.

**History rows are written inside `mutate()`, alongside the column change they
describe.** That placement is the whole of the answer to two questions the
ROADMAP bullet raised as open, and it answers them by construction rather than
by a rule this spec invents:

- **Does a dry run write history?** No, and no code is needed to prevent it.
  `commitAndRender()` rolls back under `dryRun`, so a preview commits nothing
  on either path. History is not a special case.
- **Does `seq` continue across a rolled-back transaction?** It does not have
  to. The rows roll back with the item change that accompanied them, and
  `maxHistorySeq()` reads committed state — so the next attempt sees the same
  maximum the aborted one did, and there is no gap to skip.

**Both were genuinely open when the bullet was written on 2026-08-04, and were
closed by ANTS-3809 shipping `commitAndRender()` afterwards.** Recorded so a
reader does not go looking for the decision.

### 2.3 The cap — note and continue, never fail the op

`appendHistory()` refuses when `historyBytes() + incoming > m_historyCap`
(default `kDefaultHistoryCapBytes` = 250 MiB, `src/roadmapstore.h`), returning
`false` with an error. INV-14 makes that refusal deliberate: below the bound
nothing is evicted, at the bound the write fails and reports.

**A `false` from `appendHistory()` must not abort the op.** `mutate()`
returning false makes `commitAndRender()` roll the whole transaction back, so
a full history table would turn every roadmap write into a refusal — the audit
trail taking the roadmap down with it, which is the opposite of what an audit
trail is for.

The migration already faced this and its answer is the one to copy: record a
note and carry on, with the item write standing. `Loader::recordHistory()`
distinguishes the cap refusal from any other failure by **re-evaluating the
store's own predicate** rather than matching the error string — and its
comment records that comparing `historyBytes()` against the cap alone is
wrong, because at the moment of refusal the stored bytes are still below the
cap in every case but an exact landing.

**Reuse that discriminator; do not re-derive it.** It is three public terms and
one comparison, and getting it wrong fails open in the direction that aborts
writes.

### 2.4 Creation writes no history

`append` and `append_batch` create an item through `putItem()`. **They write no
history rows**, matching the migration: `recordHistory()` is reached only from
`applyPlanFields()`, which compares a stored value against a planned one and
runs only for items that already exist.

The reasoning is that a creation's "old value" is not empty — it does not
exist — and a revision row claiming a transition from `""` invents a state the
item was never in. The item's own row is the record that it was created; the
`history` table records what changed *about* it afterwards.

### 2.5 One stamp per op, computed once

The op computes `changed_at` once, at the top of `mutate()`, as
`QDateTime::currentDateTimeUtc()` formatted to the DDL's shape. Every row the
op writes shares it.

Computing it per row would let a batch straddle a second boundary and split one
logical write across two revisions, for no benefit — and on `append_batch`,
which is N items in one transaction, it would do so routinely.

**A batch shares the stamp across items too.** `seq` is scoped per
`(item_pk, changed_at)`, so each item's rows number from their own base and the
UNIQUE constraint is satisfied without coordination between items.

## 3. Invariants

- **INV-1** — Every `roadmap_log` op that changes an existing item's column
  writes one `history` row per changed column. *Test:*
  `tests/features/roadmap_write_history` — flip an item's status through the
  verb, then `SELECT` the `history` rows for that `item_pk` and assert one row
  whose `field` is `status`, carrying the pre-flip value in `old_value`.
- **INV-2** — All rows one op writes share one `changed_at` and carry
  contiguous `seq` starting from `maxHistorySeq()+1` for that stamp (0 when
  none exists). *Test:* same suite — an `amend_body` that re-derives trailer
  columns writes ≥ 2 rows; assert one distinct `changed_at` and that the `seq`
  set is contiguous with no gap.
- **INV-3** — A `dry_run` write leaves the `history` table byte-identical.
  *Test:* same suite — count rows and `SUM(length(...))` before and after a
  `dry_run:true` flip; assert both unchanged. Breaks if a future refactor
  moves the history write outside `commitAndRender()`'s transaction.
- **INV-4** — A rolled-back op leaves no history rows and burns no `seq`.
  *Test:* same suite — force a rollback via the render gate (an item with no
  `Layman:` line makes `commitAndRender()` return `GateUnmet`), assert zero
  rows added, then let a successful write proceed and assert its first row is
  the `seq` the aborted one would have used.
- **INV-5** — At the history cap the item write still succeeds, the op still
  returns success, and the envelope carries a note. *Test:* same suite —
  construct a `RoadmapStore` with a deliberately tiny `historyCapBytes` (it is
  a constructor parameter, so no 250 MiB fixture is needed), perform a flip,
  assert the item's status changed and the op did not refuse.
- **INV-6** — `append` and `append_batch` write no history rows. *Test:* same
  suite — append an item, assert zero rows for its `item_pk`.
- **INV-7** — The export round-trips consumer-written rows unchanged. *Test:*
  the existing `roadmap_export_roundtrip` suite, extended with an item carrying
  a consumer-written revision; its byte-identity assertion is what fails if the
  two producers disagree on row shape.

## 4. RAM / build cost

No new build target, no new library, no external dependency. The writes are
`INSERT`s on an already-open connection inside an already-open transaction.

**Storage is the cost that matters, and it is bounded by an existing control.**
Each row stores `field`, `old_value` and `new_value`; `historyBytes()` sums
exactly those three and the 250 MiB cap is enforced against the total. The
`old_value` of a body edit is a whole item body, so bodies dominate.

**The order-of-magnitude check, since a cap a normal year of use reaches is a
design fault rather than a safety net.** Measured against this project's live
store on 2026-08-17:

```
$ sqlite3 ~/.local/share/ants-terminal/roadmap.sqlite \
    "select count(*), count(distinct item_pk||changed_at) from history;"
804|761
$ sqlite3 … "select cast(avg(length(field)+length(coalesce(old_value,''))
             +length(coalesce(new_value,''))) as int) from history;"
1106
$ sqlite3 … "select count(*) from item;"
2081
```

804 rows across 761 revisions, averaging 1,106 bytes — so the migration of
2,081 items consumed roughly **0.3% of the 250 MiB cap**, and the cap admits
on the order of 200,000 rows at that average.

**Two honest caveats on extrapolating that.** Those rows are the migration's,
and a migration writes an item's fields once; a consumer write records an
`old_value` that may be a long accumulated body, so the per-row average should
be expected to rise. And 761 revisions of 804 rows is 1.06 rows per revision,
which is *not* the multi-field shape § 2.1 describes — the trailer re-derivation
this spec records is a consumer-write behaviour and the migration mostly wrote
one field per item. Neither changes the conclusion: at the measured average the
cap is two orders of magnitude away, and § 2.3 defines what happens if a
project ever reaches it.

## 5. Out of scope

- **Reading history back.** A `lastTouchedAt(itemPk)` reader, and the dialog
  change that would use it to retire ANTS-4414's `git blame`, are the payoff
  and not this item — this spec is the write side alone. Deliberately split:
  the reader is worthless until rows exist, and bundling them would put a
  UI-visible change behind a data-model contract.
- **Backfilling history for edits made since migration.** Not possible — the
  old values were never recorded. `git blame` on `ROADMAP.md` is the only
  witness and it dates lines, not fields.
- **Section, element and project writes.** `history.item_pk` is `NOT NULL` and
  references `item(item_pk)`, so the table cannot hold a section revision
  without a schema change. Not done at all; no id.
- **Eviction, rotation or compaction of history.** INV-14 fixes the policy at
  never-evict, and § 2.3 keeps it. No id.
- **Exposing history through an MCP verb.** No id — nobody has asked, and the
  reader above is the first real consumer.

## 6. Tests

Feature test: `tests/features/roadmap_write_history/`. Covers INV-1..INV-6.
Label `features;fast`. Bundle per `build_target_for` — the roadmap_log verb
tests live in `test_claude`, and this drives the verb rather than the dialog.

INV-7 extends `tests/features/roadmap_export_roundtrip/` rather than adding a
second round-trip harness.

Assertions read the `history` table by direct `SELECT` rather than through a
typed reader, which is the same choice `roadmap_store_schema`'s tests make and
for the same reason: the subject is the row that landed, and routing through a
reader would assert the reader instead.

**Verify each test fails against pre-fix source first.** INV-1, INV-2 and
INV-6 fail today by returning zero rows, which is a weak red — zero is also
what a broken fixture yields. Prove the fixture first by asserting the
migration's own rows are visible to it, then assert the consumer's absence.

## 7. Cross-doc impact

- `docs/standards/roadmap-data-model.md` — its history section describes rows
  the migration writes; it gains the consumer-write case and § 2.1's
  definition of a revision.
- `docs/specs/ANTS-3809-roadmap-write-half.md` — § 2.2's eight-op table gains
  the history write per row. An amendment recording what was built, not a
  change of direction.
- `docs/specs/ANTS-3756-roadmap-store-schema.md` — INV-14's cap now has a
  second writer subject to it; § 2.3's note-and-continue rule is stated here
  and should be cross-referenced there rather than restated.
- ROADMAP.md ANTS-3822 — its "blocked by ANTS-3809" line is stale; ANTS-3809
  shipped.
- CHANGELOG.md — on ship.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
