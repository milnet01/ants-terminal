# ANTS-4501 — Roadmap report: totals, lifecycle and throughput per period

**Status:** spec draft (2026-08-19).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-4501 (user-request-2026-08-19).

## 1. Problem

A session cannot answer *"where does the roadmap stand?"* without reading the
whole of `ROADMAP.md`. The point-in-time half of the question — how many items
exist, how many are open, how many are in flight — is one aggregate query over
the store and is exposed by no verb. The time-bucketed half — closed today,
this week, this month, this year — cannot be answered at all, because **the
store records no date for anything.**

That absence is the whole design problem, and it is measured rather than
assumed.

The `item` table already carries `created`, `last_modified` and `shipped`
(date-only, nullable, `GLOB`-CHECKed to `YYYY-MM-DD`). `RoadmapStore::putItem()`
binds all three and `RoadmapStore::readItem()` reads them back, so the round
trip works. `RoadmapStore::setItemField()`'s allowlist contains all three. The
plumbing is complete; **no caller anywhere in `src/` ever supplies a value.**

```
$ sqlite3 ~/.local/share/ants-terminal/roadmap.sqlite \
    "select count(*), sum(created is not null), sum(last_modified is not null),
            sum(shipped is not null) from item;"
4816|0|0|0
```

All 4816 items across all 15 registered projects are undated on every column.

The `history` table does carry timestamped transitions and does record status
changes, so *some* closures are derivable from it. Not enough to be useful:

```
$ sqlite3 ~/.local/share/ants-terminal/roadmap.sqlite \
    "select (select count(*) from history where field='status' and new_value='shipped'),
            (select count(*) from item where status='shipped');"
67|3337
```

**Sixty-seven derivable closures against 3337 closed items — 2.0% coverage.**
The reason is structural rather than a gap that will fill: `recordHistory()` is
reached only from the field-update path, so a first migration writes no history
at all, and for every project the entire pre-migration backlog is absent. The
store's oldest history row is six days old, against a repository that starts in
April:

```
$ sqlite3 ~/.local/share/ants-terminal/roadmap.sqlite "select min(changed_at), max(changed_at) from history;"
2026-08-13T18:53:23Z|2026-08-19T09:19:40Z
$ git log --reverse --format=%ad --date=short | head -1
2026-04-03
```

So a report built on the store as it stands would answer *"closed this year"*
with a number roughly fiftyfold too small (3337 / 67 = 49.8) — and, the part
that makes this a contract rather than a feature, **it would look like an
answer.**

## 2. Surface

### 2.1 The decision: two date sources, and `history` is not one

Three designs were available (the roadmap bullet states them). This spec takes
two and rejects the third.

| Source | Answers the past? | Taken |
|---|---|---|
| Stamp the three columns on every write | no | **yes** — § 2.2 |
| Backfill from `git log` over the roadmap files | yes | **yes** — § 2.3 |
| Derive from the `history` table at query time | no (2.0%) | **no** |

**These are this session's calls, not user-accepted ones** (2026-08-19). The
request named the metrics, never the mechanism; the gate and the user are both
free to reverse them, and the reasoning below is what they would be reversing.

**Stamping is taken** because it is the only source that stays correct without
maintenance, and because the columns and their writers already exist.

**The git backfill is taken because the request requires it.** The question
asked is explicitly about the year, and 3337 of this store's items closed
before any of this existed. Stamping alone answers that question with a number
covering the last six days. The roadmap bullet flags the backfill as "the part
most likely to be underestimated"; measured, it is not large — see § 2.3.

**Deriving from `history` is rejected**, and the coverage figure is only the
first ground. `history` is a change log: a row says *a field moved*, not *when
the item closed*, and an item reopened and re-closed has several. Two answers
to one question is what `documentation.md` § 2.1 forbids of prose, and this
spec declines to build it in SQL. Where `history` and a stamped column
disagree, the column governs, because the column is what the report reads.

### 2.2 Stamping (forward)

Three rules, each on a different write path:

- **`created`** — set at row insert, when `putItem()` creates a row that did
  not exist. Never rewritten.
- **`last_modified`** — set on every successful `setItemField()` /
  `clearItemField()`, and at insert.
- **`shipped`** — set **only on the transition into `shipped`**: a `status`
  write whose new value is `shipped` and whose old value is not. It is
  *cleared* on a transition out, so a reopened item carries no closure date.

The `shipped` rule is the one with a trap, and INV-5 exists for it. Stamping
`shipped` on any write *while* the item is shipped means every re-render moves
every closed item's date to today, and "closed this week" silently becomes the
entire backlog. That failure is invisible in a unit test on one item and
obvious only at corpus scale.

**The date is the local calendar date**, formatted `YYYY-MM-DD` to satisfy the
column's existing `GLOB` CHECK. Not UTC: a user asking "what did I close
today?" means their day. § 2.4 owns the boundary rule that follows.

**Migration does not stamp.** The three columns stay outside the plan's field
set, exactly as today — `Loader::fieldsOf()` in `src/roadmapmigrateload.cpp`
omits them, and its comment names the reason: a source file cannot express
them, so a re-run must never clear one. That is **ANTS-3765 INV-3**. *(The
roadmap bullet cites `ANTS-4065 INV-3` for this rule; that INV is the backtick
guard against a quoted trailer label. The correct owner is ANTS-3765.)*

### 2.3 Backfill (historical), one-off and opt-in

A closure is observable in version control: the bullet's status marker becomes
`✅` in some commit, and that commit has a date. The roadmap files are tracked,
so the walk is — for each commit touching them, oldest first, read the set of
ids carrying a shipped marker. The first commit in which an id appears in that
set is its `shipped` date; the first commit in which it appears at all is its
`created` date.

Cost, measured on this project rather than estimated:

```
$ git rev-list --count HEAD -- ROADMAP.md docs/roadmap/
1496
$ start=$(date +%s%N); for sha in $(git rev-list -n 20 HEAD -- ROADMAP.md); do \
    git show "$sha:ROADMAP.md" | grep -c '^- . \[ANTS-'; done >/dev/null; \
  end=$(date +%s%N); echo "$(( (end-start)/1000000 )) ms"
302 ms
```

1496 revisions at that rate is roughly 23 seconds — a one-off, not a query-time
cost. This is why the backfill is a **separate explicit operation and never a
side effect of a read.**

The marker scan agrees with the store exactly, which is what makes the walk
trustworthy rather than approximate:

```
$ cat ROADMAP.md docs/roadmap/0.6.md docs/roadmap/0.5.md | grep -cP '^\s*- \x{2705} \[ANTS-'
1737
$ sqlite3 ~/.local/share/ants-terminal/roadmap.sqlite \
    "select count(*) from item where project_id=1 and status='shipped';"
1737
```

Three properties the backfill must have, each an invariant below: it never
overwrites a non-NULL date (INV-2), it never invents one for an id it did not
observe (INV-3), and it is re-runnable to the same result (INV-2).

**It is per-project**, because the walk needs that project's git repository,
and the store is machine-global across 15 of them.

### 2.4 Bucket semantics

The periods are a contract: two callers asking the same question of the same
data must get the same number.

- **Day** — the caller's local calendar date.
- **Week** — ISO-8601, Monday-start.
- **Month**, **Year** — calendar, local.
- Every bucket is **half-open**, `[start, next_start)`, so an item belongs to
  exactly one bucket at each granularity (INV-8).
- `since:"YYYY-MM-DD"` requests one explicit window instead of the four
  standard ones.

### 2.5 The report surface

`roadmap_query` gains `mode:"report"`. Not a new verb: it is a read over the
same rows, resolved by the same `caller_cwd`, and `mode:"bundles"` is the
precedent — an aggregate view with its own builder
(`RemoteControl::buildRoadmapBundlesEnvelope()`) rather than an arm inside
`RemoteControl::cmdRoadmapQuery()`, which already spans 2114 lines
(`src/remotecontrol_roadmap_query.cpp`, line 1084 to the next top-level
definition at 3198).
`mode:"report"` gets its own builder for the same reason.

The mode returns no `bullets[]`, refuses `section` / `id` / `ids` with
`bad_mode_combo` as `bundles` already does, and writes nothing.

Response shape — directional, not exhaustive. **The figures are this project's
on 2026-08-19**, present so the shape reads against real data; they are a dated
sample and not a claim about now:

```jsonc
{
  "ok": true,
  "mode": "report",
  "scope": "project",          // or "all"
  "generated_for": "2026-08-19",
  "totals":  { "items": 2143, "open": 406, "in_progress": 4, "shipped": 1737 },
  "by_status": { "planned": 329, "in-progress": 4, "shipped": 1737, "considered": 73 },
  "by_kind":   { "fix": 0, "feature": 0 },
  "periods": {
    "day":   { "closed": 0, "added": 0, "net": 0 },
    "week":  { "closed": 0, "added": 0, "net": 0 },
    "month": { "closed": 0, "added": 0, "net": 0 },
    "year":  { "closed": 0, "added": 0, "net": 0 }
  },
  "coverage": {
    "shipped_dated": 0, "shipped_undated": 1737,
    "created_dated": 0, "created_undated": 2143
  },
  "age_open": { "median_days": null, "oldest_days": null, "over_90": null, "sample": 0 },
  "time_to_close": { "median_days": null, "sample": 0 }
}
```

**`open` is every item whose status is not `shipped`** — planned, in-progress and
considered together — and `in_progress` is the subset of it that is in flight,
reported beside it rather than instead of it. Stated because the two readings
differ by the `considered` count (73 of this project's 2143) and both look
reasonable, so a caller comparing two runs could not tell a definition change
from a real one.

`coverage` is not optional detail. It is the block that stops every other
number lying, and INV-1 requires it.

### 2.6 Metrics beyond the request

The roadmap bullet lists further metrics. This spec takes the four that are one
aggregate over columns that already exist — `by_status`, `by_kind`, `net` per
period, `age_open` — and defers the rest to § 5. A metric needing a join the
schema does not carry is a second feature wearing this one's id.

## 3. Invariants

- **INV-1** — **Every bucketed figure ships with the count of rows that could
  not be bucketed.** A `periods` block is never emitted without a `coverage`
  block over the same population. *Test:* `roadmap_report` builds a store with
  3 dated and 7 undated shipped items, asserts `periods.year.closed == 3`
  **and** `coverage.shipped_undated == 7`. *Breaks when:* the query is written
  as `WHERE shipped IS NOT NULL` and reports the survivors as the answer —
  which is the natural SQL, reads as correct, and turns a 2% sample into a
  confident total.

- **INV-2** — **The backfill never overwrites a non-NULL date, so it is
  re-runnable.** An item whose `shipped` was set by stamping, or by an earlier
  backfill, keeps it. *Test:* `roadmap_report` stamps `shipped='2026-01-01'`,
  runs a backfill that would derive `2026-05-05`, asserts the value is still
  `2026-01-01`, then runs the backfill twice more and asserts no column moves.
  *Breaks when:* the backfill writes unconditionally — which makes a second run
  a different operation from the first and destroys any human correction.

- **INV-3** — **The backfill writes only dates it observed in a commit.** An id
  present in the store but never seen in any revision of the roadmap files is
  left NULL and counted in `coverage`, never given the walk's first or last
  date as a fallback. *Test:* `roadmap_report` runs a backfill over a fixture
  repository whose history omits one stored id, asserts that id's three columns
  are still NULL. *Breaks when:* an unmatched id inherits a boundary commit's
  date, which afterwards is indistinguishable from a real one.

- **INV-4** — **A re-migration never clears a stamped date.** After stamping,
  `roadmap_migrate` over the same source leaves all three columns untouched.
  *Test:* `roadmap_migrate_load` stamps the three columns via `setItemField()`,
  re-loads the same plan, asserts all three survive — the shape ANTS-3765
  INV-3 already uses for `milestone`. *Breaks when:* the three columns are
  added to `Loader::fieldsOf()` so the plan can express them; the plan cannot,
  so every one would arrive empty and the empty-does-not-overwrite rule would
  be the only thing between a stamp and its deletion.

- **INV-5** — **`shipped` is stamped on the transition into shipped, never on a
  write to an already-shipped item.** *Test:* `roadmap_report` ships an item
  (asserting the date), writes its `body` on a later simulated day, and asserts
  `shipped` is unchanged. *Breaks when:* the stamp is attached to "status is
  shipped" rather than "status became shipped" — after which one re-render
  dates the whole backlog to today and every throughput figure is wrong in the
  same direction, which is what makes it hard to notice.

- **INV-6** — **`shipped` is cleared on a transition out of shipped.** *Test:*
  `roadmap_report` ships an item and asserts `shipped` is **non-NULL**, then
  flips it back to `planned` and asserts it is NULL. The first assertion is what
  makes the clause mean anything: without it the fixture reads NULL before and
  NULL after, and passes against a build that implements neither half of § 2.2's
  `shipped` rule. *Breaks when:* only the set half is implemented, so a reopened
  item counts as closed in every period report thereafter.

- **INV-7** — **The report's point-in-time totals equal the store's own row
  counts.** `totals.items` and every `by_status` entry match `SELECT COUNT(*)`
  over the same project. *Test:* `roadmap_report` asserts the mode's
  `by_status` equals a direct count query on the same fixture. *Breaks when:*
  the report filters by something the count does not — an orphan predicate, a
  visibility default — so two verbs describe one roadmap differently.

- **INV-8** — **Buckets are half-open, so an item falls in exactly one at each
  granularity.** *Test:* `roadmap_report` dates one item to the last day of a
  month and one to the first of the next, asserts each is counted once and that
  the two month buckets sum to the pair. *Breaks when:* both ends are
  inclusive, double-counting every boundary date.

- **INV-9** — **Every median ships the sample it was computed from.**
  `age_open` and `time_to_close` each carry `sample`, and it is the count the
  median used, not the population. *Test:* `roadmap_report` asserts
  `time_to_close.sample` equals the number of items with both dates known, on a
  fixture where that is smaller than the shipped count. *Breaks when:* the
  sample reports the population, so a median over four items reads as a trend
  across two thousand.

- **INV-10** — **`mode:"report"` writes nothing.** No row, no file, no
  migration, no id allocation. *Test:* `roadmap_report` hashes the store file,
  issues the report, asserts the hash is unchanged. *Breaks when:* the report
  reaches for the migration path to freshen the rows it is about to summarise —
  tempting, because a stale store gives a stale report.

## 4. RAM / build cost

The report is aggregate SQL over `item`; its result is bounded by the number of
distinct status and kind values, not by the item count. No new target, no new
library.

**The backfill is the memory question.** The walk holds one revision's id set
at a time — roughly 2000 short strings at this project's largest revision —
plus two accumulating maps, `id → first-seen date` and `id → first-shipped
date`, bounded by the project's item count (2143 here, 4816 store-wide). It
must **not** retain each revision's file contents: that is 1496 × ~4 MB and is
the shape that turns a 23-second job into an OOM kill on this host. The
accumulating maps are the only structure that grows with the walk, and they are
bounded by item count rather than by revision count.

## 5. Out of scope

- **Store vs markdown divergence per project** — needs ANTS-4488's read-only
  sync check, which does not exist. Tracked by ANTS-4488.
- **Blocked-by-format counts** (open items missing a kind or layman line) —
  doubles as a pre-flight for ANTS-4483's render gate and belongs with that
  gate. Tracked by ANTS-4483.
- **Per-lane counts** — `lanes` is a JSON array column; counting by lane needs
  a join table or JSON extraction over every row, which is a schema question
  rather than a report one. No id; file one if it is wanted.
- **Backfilling `last_modified`** — not done at all. A commit touching a bullet
  is a modification, so every commit would rewrite it, and the column's only
  consumer is "what changed recently", which `history` answers properly for the
  window it covers.
- **A dashboard, a chart, or a Roadmap-dialog surface** — not done at all. This
  is a verb that returns numbers.
- **Reopen counts and churn** — derivable from `history` only once its coverage
  is real, which is years away for existing projects. No id.

## 6. Tests

Feature test: `tests/features/roadmap_report/`. Covers INV-1..INV-10 except
INV-4, which extends the existing `tests/features/roadmap_migrate_load/`
because it asserts a migration behaviour and belongs beside ANTS-3765 INV-3's
own test. Label `features`. Ask `build_target_for` which bundle owns each new
source; the bundle is not guessable from the path.

Verify each test fails against pre-change source first. INV-5 and INV-6 are the
two whose red run matters most: both pass trivially against a build that stamps
nothing, because a NULL column and an unchanged column look identical. Their
fixtures must assert a *value*, not merely that nothing moved.

## 7. Cross-doc impact

- `docs/standards/mcp-behavioural-notes.md` — a `roadmap_query` per-verb note
  for `mode:"report"`, and the backfill's one-off nature.
- `docs/specs/ANTS-3756-roadmap-store-schema.md` — § 7's method census moves if
  the store gains an aggregate reader, and the three date columns stop being
  write-only.
- `docs/specs/ANTS-3765-roadmap-migration-load.md` — INV-3's enumeration names
  `milestone`, `resolution`, `visibility` and `priority` but not the three
  dates, while `Loader::fieldsOf()`'s comment does name them. Widen the
  invariant's list; its claim already covers them.
- `CHANGELOG.md` — a user-visible new mode.
- `CLAUDE.md` — no change; the live verb catalogue is `tool_info {catalog:true}`.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
