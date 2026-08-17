# ANTS-3822 — consumer writes append a history row

**Status:** spec draft (2026-08-17).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3822 (in-session-2026-08-04, ANTS-3809 cold-eyes loop 1 lane A; picked up 2026-08-17 on user request after ANTS-4414 measured what its absence costs).
**Blocked by:** none — ANTS-3809 shipped, which is the write path this hooks into.
**Blocker for:** the store-backed last-touch reader that would retire ANTS-4414's `git blame`.

## 1. Problem

`RoadmapStore` has a `history` table and an `appendHistory()` writer, and
**not one `roadmap_log` write op populates either.** They change the item
column and move on, so the audit trail has a hole that opens the moment a
project migrates.

**"Eight ops" is `ANTS-3809` § 2.2's count and is already stale — there are
nine write paths.** Counted rather than recalled, 2026-08-17:

```
$ awk '/^### 2.2 The eight ops/,/^### 2.3/' \
    docs/specs/ANTS-3809-roadmap-write-half.md | grep -cE '^\| `[a-z_]+` \|'
8
$ rg -n 'commitAndRender' src/remotecontrol_roadmap_log.cpp | grep -v '^\S*: *//' | wc -l
9
```

`ANTS-4070` added `rotate_minor` and `retitle_section` after that table was
written. **Both are section-only** — they write `section` rows through
`setSectionSlug()` / `setSectionSource()` / `updateSection()` — so § 5 puts them
out of scope by schema rather than by choice.

Nine call sites for ten ops because **`flip` and `annotate` share
`cmdRoadmapLogFlip`**, which `ANTS-3809` § 2.2 states outright; the two
`ANTS-4070` ops each have their own. Resolved by mapping every site to its
handler rather than inferring which pair shared:

```
391 cmdRoadmapLogAppend   1207 cmdRoadmapLogFlip (+annotate)  2297 cmdRoadmapLogAmendBody
2979 cmdRoadmapLogFlipBatch   3606 cmdRoadmapLogCreateSection  3955 cmdRoadmapLogBundleRow
4741 cmdRoadmapLogAppendBatch  5275 cmdRoadmapLogRotateMinor   5400 cmdRoadmapLogRetitleSection
```

**This spec therefore governs the four ops that write an ITEM column —
`flip`, `annotate`, `flip_batch`, `amend_body` — and the total count is not the
load-bearing fact**; § 2.4 and § 5 name every exclusion by op.

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
time.** Every `flip`, `flip_batch`, `annotate` and `amend_body` since is
invisible. A field's previous value is recoverable for a migrated write and
not for a consumer one, which inverts the usual expectation that recent
changes are the best-recorded.

**That list is the four ops this spec makes visible, and it deliberately
excludes `append` / `append_batch`** — a creation has no previous value to
lose, so its absence from `history` is correct rather than a hole. § 2.4 owns
the reasoning.

**The data model already says the store is where this history is supposed to
live**, which is what makes the gap a defect rather than an unbuilt feature.
`roadmap-data-model.md` § 6: *"git currently carries the history of every
roadmap edit because the roadmap is a tracked file, and under this model the
store is untracked and the render lossy, so that history would otherwise have
nowhere to live."* The store being untracked is already true. So the history
does have nowhere to live, for every edit since migration.

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
`roadmap-data-model.md` § 6 defines the record type as **"one row per field
change"**, and `Loader::applyPlanFields()` implements exactly that: one row per
changed column, all sharing one `changed_at`, with `seq` incrementing across
them. The table's `UNIQUE (item_pk, changed_at, seq)` is what makes that
addressable.

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

**An absent prior value is SQL `NULL`, never `''`.** `old_value` and `new_value`
are nullable and `appendHistory()` preserves the distinction deliberately — it
binds a null `QVariant` when the `QString` `isNull()`. So a column that held
nothing before the write records `NULL`, which is what the migration's rows
carry. Writing `''` instead would be a second convention visible in the export
record, and INV-7's byte-identity assertion would fail without saying which
producer was wrong.

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

### 2.3 The cap — note and continue; every OTHER failure still aborts

`appendHistory()` refuses when `historyBytes() + incoming > m_historyCap`
(default `kDefaultHistoryCapBytes` = 250 MiB, `src/roadmapstore.h`), returning
`false` with an error. INV-14 makes that refusal deliberate: below the bound
nothing is evicted, at the bound the write fails and reports.

**A CAP refusal must not abort the op. Every other `false` must.** Both halves
are required and neither is the default:

- **Cap refusal** → record a note, carry on, item write stands. `mutate()`
  returning false makes `commitAndRender()` roll the whole transaction back, so
  treating a full history table as a failure would turn every roadmap write
  into a refusal — the audit trail taking the roadmap down with it, which is
  the opposite of what an audit trail is for.
- **Any other `false`** — a constraint violation, a closed database, disk
  failure — **fails `mutate()` and aborts the write**, exactly as
  `Loader::recordHistory()` does. Swallowing these is the silently-dropped
  revision INV-14 exists to forbid, and it would be reported as a successful
  write.

**That is why a discriminator is needed at all**, and why it must be exact: the
two branches do opposite things. `Loader::recordHistory()` tells them apart by
**re-evaluating the store's own predicate** rather than matching the error
string — and its comment records that comparing `historyBytes()` against the
cap alone is wrong, because at the moment of refusal the stored bytes are still
below the cap in every case but an exact landing.

**The discriminator gets one home on `RoadmapStore`, and this spec names it
rather than leaving "reuse it" to be interpreted.** Today it is four lines
inside a private method of `Loader`, unreachable from the consumer write path,
so "reuse" without a named home means one builder promotes it and another
copies it:

```cpp
// Would appendHistory(field, oldValue, newValue) refuse on the cap?
// Public because two writers need the same answer: the migration loader and
// the consumer write path. The three terms are already public; what was not
// public is the comparison, which is the part that is easy to get wrong.
bool historyWouldExceedCap(const QString &field, const QString &oldValue,
                           const QString &newValue) const;
```

`Loader::recordHistory()` is refactored onto it in the same change, so there is
one implementation rather than two agreeing by inspection.

**The note is a response-envelope field, so this spec pins its name and shape.**
Leaving it as "a note" means the implementer invents a key that the test, any
MCP consumer and `ANTS-3756`'s amendment all then bind to:

```json
"history_note": "history cap reached (262144000 bytes); 3 revision(s) not recorded"
```

- **Key `history_note`, a string, absent when nothing was skipped.** Absent
  rather than empty, matching how this project's envelopes carry optional
  advisories.
- **One per op, not one per item.** A `flip_batch` that crosses the cap emits
  one note carrying the count, because the caller's decision — the history is
  full, go raise the cap — is the same whichever item tripped it.
- **It is an advisory on a SUCCESSFUL envelope, not a refusal.** So it is not a
  `code` from `mcp-error-codes.md`; that taxonomy is for refusals, and this op
  did not refuse.

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
- **INV-2** — All rows one op writes **for one `item_pk`** share one
  `changed_at` and carry contiguous `seq` from `maxHistorySeq()+1` for that
  stamp (0 when none exists). **`seq` is scoped per `(item_pk, changed_at)`, so
  a batch does NOT carry one op-wide counter** — each item numbers from its own
  base. *Test:* same suite, two legs, because one cannot separate the readings:
  an `amend_body` that re-derives trailer columns writes ≥ 2 rows for one item —
  assert one distinct `changed_at` and a contiguous `seq` set; then a
  `flip_batch` over two items that each already carry a revision at that stamp —
  assert each item's rows resume from **its own** maximum. A single-item leg is
  green against an op-wide counter, which is why the batch leg is required.
- **INV-3** — A `dry_run` write leaves the `history` table byte-identical.
  *Test:* same suite — count rows and `SUM(length(...))` before and after a
  `dry_run:true` flip; assert both unchanged. Rests on
  `commitAndRender()` returning `abort(Result::Ok)` under `dryRun`
  (`src/roadmapwrite.cpp`, step 5), so it breaks if a future refactor moves the
  history write outside that transaction.
- **INV-4** — A rolled-back transaction leaves no history rows. *Test:* same
  suite, driven at the **store** level: `begin()`, `setItemField()` plus its
  history row, `rollback()`, then assert zero rows for that `item_pk` and that
  `maxHistorySeq()` is unchanged. **Deliberately not driven through the render
  gate:** `commitAndRender()`'s gate is *project*-scoped — its own message is
  "the roadmap render refuses this project: %1 open item(s) carry no `Layman:`
  line" — so while the offending item is still open every later write in that
  project also refuses, and a two-phase recipe through the verb cannot reach its
  second phase. **The "burns no `seq`" half is dropped rather than asserted:**
  `seq` is scoped per `(item_pk, changed_at)` and each op stamps its own second,
  so a retry normally gets a fresh stamp and starts at 0 whether or not the
  aborted attempt burned anything — the assertion would pass against both a
  correct and an incorrect implementation.
- **INV-5** — At the history cap the item write still succeeds, the op returns
  success, and the envelope carries `history_note`. *Test:* same suite, two
  legs, because the cap and the envelope are reachable from different places.
  **The cap leg calls `RoadmapWrite::commitAndRender()` directly**, handing it a
  `RoadmapStore` built with a deliberately tiny `historyCapBytes` — a
  constructor parameter, so no 250 MiB fixture is needed. A verb-driven test
  cannot do this: `src/roadmapsource.cpp` constructs the verb path's store with
  `kDefaultHistoryCapBytes` and takes no injection, so `commitAndRender()` is
  the outermost seam that accepts a store. Assert the item's field changed and
  the call returned `Result::Ok`. **The envelope leg** asserts the verb emits
  `history_note` when the write helper reports a skip — without it an
  implementation that silently drops the revision passes, which is the exact
  failure INV-14's "fails *and reports*" forbids.
- **INV-8** — A non-cap `appendHistory()` failure aborts the op. *Test:* same
  suite — force a failure that is not the cap (write a history row for an
  `item_pk` no `item` row has, violating the foreign key), assert
  `commitAndRender()` does **not** return `Result::Ok` and that the item column
  is unchanged. Without this leg § 2.3's two branches have one test between
  them, and the dangerous branch is the untested one.
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

Feature test: `tests/features/roadmap_write_history/`. Covers INV-1..INV-6 and
INV-8. Label `features;fast`. Bundle per `build_target_for` — the roadmap_log
verb tests live in `test_claude`, and this drives the verb rather than the
dialog.

**Two invariants are driven below the verb, and that is a requirement rather
than a convenience.** INV-4 and INV-5's cap leg call
`RoadmapWrite::commitAndRender()` directly, because it is the outermost entry
point that accepts a caller's `RoadmapStore` — the verb path builds its own with
`kDefaultHistoryCapBytes` and offers no injection point. An implementer who
writes both at verb level will find INV-5's precondition unreachable and INV-4's
recipe blocked by the project-scoped gate.

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
- `docs/specs/ANTS-3809-roadmap-write-half.md` — § 2.2's table gains the history
  write on **the four item-column rows only** (`flip`, `flip_batch`,
  `annotate`, `amend_body`). Not "per row": `append` / `append_batch` are
  excluded by § 2.4, and `create_section` / `bundle_row` cannot hold a revision
  at all per § 5, so instructing a history write on all eight would instruct one
  the schema refuses. Its heading is also stale — `ANTS-4070` added two ops
  since, and § 1 records the recount.
- `docs/specs/ANTS-3756-roadmap-store-schema.md` — INV-14's cap now has a
  second writer subject to it, and its public surface gains
  `historyWouldExceedCap()` (§ 2.3), which is the twenty-fifth method on the
  surface that spec's § 2.4 enumerates. § 2.3's note-and-continue rule is
  stated here and should be cross-referenced there rather than restated.
- ROADMAP.md ANTS-3822 — its "blocked by ANTS-3809" line is stale; ANTS-3809
  shipped.
- CHANGELOG.md — on ship.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-17 | 3, cold — genre pinned `spec`; one byte-stable shared packet carrying the `history` DDL, `appendHistory()`, `historyBytes()`, `Loader::recordHistory()` + its seq priming, `commitAndRender()` lines 43–100, INV-14's declaration, ANTS-3809 § 2.2's table and every `history` mention in `roadmap-data-model.md` | **Q1 1 · Q2 4 · Q3 3 · Q4 3** (11 verified / 0 dismissed) | **Eleven verified, eleven fixed. First gate on this document.** **All three lanes independently found the same defect**, which is the strongest signal in the run: § 2.3 said *"A `false` from `appendHistory()` must not abort the op"* unqualified, under a heading reading *"never fail the op"*, while the same section told the implementer to reuse a discriminator that exists only to treat the two classes differently. One reading swallows a constraint violation and reports a successful write — the silently-dropped revision INV-14 exists to forbid; the other aborts every roadmap write when the table fills. Now two explicit branches plus **INV-8**, added because the dangerous branch had no test. **All three also found the note was unspecified** — INV-5 asserted "the envelope carries a note" and nothing named the key, its type or its text, which is a response-envelope contract the test, the MCP consumers and ANTS-3756's amendment all bind to; pinned as `history_note`, one per op, absent when nothing was skipped, explicitly not an `mcp-error-codes.md` code. **Two lanes found INV-4's recipe unreachable**: it forced a rollback through the render gate, and that gate is *project*-scoped, so while the offending item is open every later write in the project also refuses and the two-phase test can never reach phase two — INV-4 now drives the store directly. **Three Q4s were tests that could not fail**: INV-4's "burns no `seq`" is green against a correct *and* an incorrect implementation because each op stamps its own second, so the retry starts at 0 either way (dropped rather than patched); INV-5's injected cap is unreachable from a verb-driven test because `src/roadmapsource.cpp` builds the verb path's store with `kDefaultHistoryCapBytes` and takes no injection (re-aimed at `commitAndRender()`, the outermost seam accepting a caller's store); and INV-2's single-item leg cannot distinguish a per-item `seq` from an op-wide counter (batch leg added). **Two Q2s were internal contradictions I had written**: § 1 listed `append_batch` among the writes whose invisibility is the defect while § 2.4 and INV-6 require it to stay invisible, and § 7 told a sibling spec to document a history write on all eight rows when two of them are excluded by § 2.4 and two more cannot hold a revision at all. **One Q3**: an absent prior value could be `NULL` or `''`, a distinction `appendHistory()` preserves deliberately and the export's byte-identity would expose — now pinned to `NULL`. **The one Q1 was mine, found while verifying the lanes' open questions rather than by a lane**: "eight write ops" is `ANTS-3809`'s count and is stale — there are nine call sites for ten ops, `ANTS-4070` having added two section-only ops. Mapping every site to its handler also **refuted my own first repair**, which guessed that one of the new ops shared a handler; the sharing is `flip`/`annotate`. **Three lane open questions resolved clean and are not in the tally** — `commitAndRender()` does roll back under `dryRun` (`abort(Result::Ok)`, step 5), every op does route through it (9 of 9 sites), and INV-8's foreign-key trigger does refuse (`PRAGMA foreign_keys = ON` at `roadmapstore.cpp:135`, confirmed against SQLite directly). All three lanes could not verify the first of those **because my packet window stopped at step 4b** — a packet defect, not a document one. |
