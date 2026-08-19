# ANTS-4501 — Roadmap report: totals, lifecycle and throughput per period

**Status:** accepted — review-contract loops 1–2 folded, converged by cap (2026-08-19).
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

- **`created`** — set at row insert on the **store-write path only**; the
  migration loader is exempt, as it is for `last_modified`. Never rewritten.
  The exemption is load-bearing rather than tidy: `Loader::rebuildElements()` reaches
  `RoadmapStore::putItem()` too, so a rule phrased as "set at every insert"
  would stamp the migration day onto all 4816 existing rows — and INV-2's
  never-overwrite guard would then make that false date permanent. A
  pre-existing item gets its `created` from the backfill (§ 2.3) or not at
  all.
- **`last_modified`** — set on every successful `setItemField()` /
  `clearItemField()`, and at insert. **The stamp lives in the callers, not
  inside `setItemField()`, and the migration loader and the backfill are both
  exempt.** Put inside the store method it would fire on every path that
  reaches it, so one re-migration or one backfill would date every item to
  today — which is the `last_modified` rewrite § 5 rejects, and it would break
  INV-4 as well.
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

**Every path that needs "today" reads it through one injectable seam, not from
`QDate::currentDate()` directly — the stamping paths AND the report builder.**
The report's half is easy to overlook and just as load-bearing: `generated_for`
and every bucket boundary in § 2.4 are computed on the read path, so a seam
wired only into stamping leaves INV-1's and INV-8's fixtures dependent on
whatever real year the suite runs in, and flaking at every period boundary. This is a contract rather than a convenience:
INV-5 and INV-6 both assert that a *later* write does or does not move a date,
and with a single real clock both writes land on the same day, so those tests
pass against the broken build they exist to catch. The seam is a static
test-only setter — the shape `RemoteControl` already uses for
`setRoadmapHistoryCapForTest()` — and is never reachable from a request.

**Migration does not stamp — on an update or an insert.** The three columns
stay outside the plan's field set, exactly as today — `Loader::fieldsOf()` in `src/roadmapmigrateload.cpp`
omits them, and its comment names the reason: a source file cannot express
them, so a re-run must never clear one. That is **ANTS-3765 INV-3**. That
argument covers an *update* only; the insert half is § 2.2's `created` bullet,
which exempts the loader by name — without both, `Loader::rebuildElements()`
would stamp today onto every row it creates. *(The
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
$ start=$(date +%s%N); for sha in $(git rev-list -n 20 HEAD -- ROADMAP.md docs/roadmap/); do \
    for f in ROADMAP.md docs/roadmap/0.6.md docs/roadmap/0.5.md; do \
      git show "$sha:$f" 2>/dev/null | grep -c '^- . \[ANTS-'; done; done >/dev/null; \
  end=$(date +%s%N); echo "$(( (end-start)/1000000 )) ms"
228 ms
```

The sample reads all three roadmap files per revision, which is what the walk
does; `git rev-list` returns 1496 for `ROADMAP.md` alone and for the three
together, so the sample and the population are the same revision set. 1496
revisions at that rate is roughly 17 seconds — a one-off, not a query-time
cost. This is why the backfill is a **separate explicit operation and never a
side effect of a read.**

**Its surface is `roadmap_log op:"backfill_dates"`, and explicitly not a
`roadmap_query` mode.** It writes, and `roadmap_query` is the read verb — INV-10
forbids the report from writing at all, so putting a writing operation behind
the same verb would contradict the invariant one section along. It takes
`caller_cwd` (the project whose git history is walked — one project per call,
like `roadmap_migrate`) and `dry_run`, and returns the counts it would write
plus the ids it could not date. It refuses `not_a_git_repo` when the project
root has no git history to walk, and `project_not_registered` when the store
holds no row for that root.

**The date taken from a commit is its AUTHOR date (`git log --date=short
--format=%ad`), in local time.** Pinned because § 2.4 promises two callers the
same number from the same data, and the committer date moves under every rebase
and cherry-pick while the author date does not — so a rewritten history would
otherwise change every figure this report produces.

The marker scan agrees with the store exactly, which is what makes the walk
trustworthy rather than approximate:

```
$ cat ROADMAP.md docs/roadmap/0.6.md docs/roadmap/0.5.md | grep -cP '^\s*- \x{2705} \[ANTS-'
1737
$ sqlite3 ~/.local/share/ants-terminal/roadmap.sqlite \
    "select count(*) from item where project_id=1 and status='shipped';"
1737
```

Four properties the backfill must have, each an invariant below: it never
overwrites a non-NULL date (INV-2), it never invents one for an id it did not
observe (INV-3), it is re-runnable to the same result (INV-2), and **it skips the `shipped`
stamp — that column alone — for any id whose current status is not `shipped`**
(INV-3). `created` is still written for every id the walk observes, whatever
its status. Stated at column granularity because skipping the whole id would
leave every open item undated, and `added` and `age_open` are then unanswerable
for exactly the backlog the request asks about.

That fourth one is not obvious and is the reason it is written down. § 2.2
*clears* `shipped` when an item is reopened — so its column is NULL, the
never-overwrite rule does not protect it, and git still holds the commit where
its marker was `✅`. A backfill that reasons only from git therefore re-closes
every reopened item, silently, on every run.

**It is per-project**, because the walk needs that project's git repository,
and the store is machine-global across 15 of them.

**Amended 2026-08-20, at implementation (write-spec Step 8). The walk reads
DIFFS, not the content of every revision, and the cost model above is a lower
bound rather than the figure.** Reading each revision's content for real is
49 s of `git show` alone on this repository (1525 revisions × 3 files, measured)
before one bullet is parsed — and the MCP bridge times out at 60 s (ANTS-3444),
so the op as costed could not complete through its own surface. One
`git log --reverse -p -U0` over the same pathspecs is 3.9 s end to end,
including the parse.

The semantics are the same because only a FIRST sighting is ever recorded: an id
first appears in the `+` line that added its bullet, and first carries a shipped
marker in the `+` line that flipped it. A later re-addition — an archive rotation
re-emitting a ✅ bullet into a new file — is already seen and changes nothing,
which is the first-wins rule a content walk applies too. Merges are diffed
against their first parent (`--diff-merges=first-parent`), or a bullet reaching a
branch only through one would be invisible; this repository has no such commit
and other projects on this store do. Each commit's added bullet lines go to
`RoadmapParse::parseBullets()` as a document, so ANTS-3808 INV-2's one grammar
still holds and no second regex learns the bullet shape.

Measured against this project 2026-08-20 by the suite's own dry run: 1525
revisions in 3.9 s, 2179 of 2179 items dated, **0 undated** — and the 1754
`shipped` writes equal the store's own `status='shipped'` count exactly, which is
the walk agreeing with the store rather than approximating it.

**A third refusal, `git_failed`**, joins the two above: git failed to start,
exited non-zero, or ran past the wall budget. A partial walk is
indistinguishable from a complete one at the caller, and every id whose commit
had not been reached would be reported undated — so a failed run refuses and
nothing is written.

### 2.4 Bucket semantics

The periods are a contract: two callers asking the same question of the same
data must get the same number.

- **Day** — the caller's local calendar date.
- **Week** — ISO-8601, Monday-start.
- **Month**, **Year** — calendar, local.
- Every bucket is **half-open**, `[start, next_start)`, so an item belongs to
  exactly one bucket at each granularity (INV-8).
- **`net` is `added - closed`.** Positive means the backlog grew over the
  bucket. The sign is pinned because both readings are natural and a caller
  seeing `net: -12` cannot otherwise tell a shrinking backlog from a growing
  one.
- `since:"YYYY-MM-DD"` requests one explicit window instead of the four
  standard ones. It replaces the whole `periods` object with a single
  `periods.since` entry of the same `{closed, added, net}` shape, so a caller
  never has to tell an absent bucket from an empty one. It takes an optional
  `until`, and the window is `[since, until)` — half-open at the top like every
  standard bucket, so two adjacent windows tile without overlapping. `until`
  defaults to tomorrow's boundary, which makes the default window include
  today.
- `scope:"project"` (default) or `scope:"all"` selects one project or every
  registered one. The store is machine-global over 15 projects, so the
  cross-project view is the one thing no single roadmap file can give; it is
  opt-in because the default question is about the project you are standing in.
  **`scope:"all"` returns the same flat envelope with every figure summed
  across projects** — not a per-project array. The per-project breakdown the
  roadmap bullet also asks for is a different response shape and is deferred
  in § 5.

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
  "by_kind":   { "fix": 604, "implement": 523, "enhancement": 323, "…": 0 },
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

**`open` is `planned + in-progress + considered`** — an enumeration, never
`status != 'shipped'`. The `item` table's CHECK admits a fifth value,
`dropped`, and the two forms disagree about it: a dropped item is not
outstanding work and is not open. `in_progress` is the subset of `open` that is
in flight, reported beside it rather than instead of it.

**`dropped` appears in `totals.items` and in `by_status`, and in no other
figure.** So `totals.items` is the sum of every `by_status` entry, and `open` is
that sum less `shipped` and `dropped`. Stated as an identity because a caller
comparing two runs could otherwise not tell a definition change from a real one.
This project has no dropped items today, which is exactly why the sample below
cannot settle it.

`coverage` is not optional detail. It is the block that stops every other
number lying, and INV-1 requires it.

### 2.6 Metrics beyond the request

The roadmap bullet lists further metrics. This spec takes the five that are one
aggregate over columns that already exist — `by_status`, `by_kind`, `net` per
period, `age_open` and `time_to_close` — and defers the rest to § 5. A metric
needing a join the schema does not carry is a second feature wearing this
one's id.

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
  are still NULL — **and, in the same fixture, an id whose history shows it
  closed but whose stored status is `planned`, asserting its `shipped` is still
  NULL after the run.** That second row is § 2.3's skip rule, and it is the one
  a build passes by accident: the first row alone is satisfied by any backfill
  that simply finds nothing. *Breaks when:* an unmatched id inherits a boundary
  commit's date, which afterwards is indistinguishable from a real one; or a
  reopened item is silently re-closed from git on the next run.

- **INV-4** — **A re-migration never clears a stamped date.** After stamping,
  `roadmap_migrate` over the same source leaves all three columns untouched.
  *Test:* `roadmap_migrate_load` stamps the three columns via `setItemField()`,
  re-loads the same plan, asserts all three survive — the shape ANTS-3765
  INV-3 already uses for `milestone`. *Breaks when:* the three columns are
  added to `Loader::fieldsOf()` so the plan can express them; the plan cannot,
  so every one would arrive empty and the empty-does-not-overwrite rule would
  be the only thing between a stamp and its deletion.

- **INV-5** — **`shipped` is stamped on the transition into shipped, never on a
  write to an already-shipped item.** *Test:* `roadmap_report` ships an item and
  asserts `shipped` equals the seam's date, **advances § 2.2's injectable
  "today" by one day**, writes the item's `body`, and asserts `shipped` is
  unchanged. The advance is what makes the clause mean anything: on one real
  clock both writes land on the same date and the assertion holds against the
  broken build. *Breaks when:* the stamp is attached to "status is
  shipped" rather than "status became shipped" — after which one re-render
  dates the whole backlog to today and every throughput figure is wrong in the
  same direction, which is what makes it hard to notice.

- **INV-6** — **`shipped` is cleared on a transition out of shipped.** *Test:*
  `roadmap_report` ships an item and asserts `shipped` is **non-NULL**, then
  flips it back to `planned` on an advanced § 2.2 seam and asserts it is NULL. The first assertion is what
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
  granularity.** *Test:* `roadmap_report` dates one item to the last day
  of a month and one to the first of the next, then takes **two `since`/`until`
  windows** covering those two months and asserts each item falls in exactly one
  and that the two windows sum to the pair. Two windows rather than two month
  buckets: the envelope emits a single `periods.month`, so the standard buckets
  cannot express the boundary at all and the clause would be unfalsifiable
  against them. *Breaks when:* both ends are
  inclusive, double-counting every boundary date.

- **INV-9** — **Every median ships the sample it was computed from.**
  `age_open` and `time_to_close` each carry `sample`, and it is the count the
  median used, not the population. *Test:* `roadmap_report` asserts
  `time_to_close.sample` equals the number of items with both dates known, on a
  fixture where that is smaller than the shipped count. *Breaks when:* the
  sample reports the population, so a median over four items reads as a trend
  across two thousand.

- **INV-10** — **`mode:"report"` writes nothing.** No row, no file, no
  migration, no id allocation. *Test:* `roadmap_report` records the row count
  of every table and the maximum `history_id`, issues the report, and asserts
  both are unchanged. **Not a hash of the store file:** the store opens in WAL
  mode (`RoadmapStore::enableWal()`), so a write lands in the `-wal` sidecar and
  leaves the main file's bytes alone until a checkpoint — a file hash would come
  back green over a report that had written rows, and could come back red over
  one that had written none. *Breaks when:* the report
  reaches for the migration path to freshen the rows it is about to summarise —
  tempting, because a stale store gives a stale report.

## 4. RAM / build cost

The report is aggregate SQL over `item`, issued by the new `RoadmapStore`
reader § 7 names; its result is bounded by the number of distinct status and
kind values, not by the item count. No new target, no new
library.

**The backfill is the memory question.** The walk holds one revision's id set
at a time — roughly 2000 short strings at this project's largest revision —
plus two accumulating maps, `id → first-seen date` and `id → first-shipped
date`, bounded by the project's item count (2143 here, 4816 store-wide). It
must **not** retain each revision's file contents: that is 1496 × ~4 MB and is
the shape that turns a 17-second job into an OOM kill on this host. The
accumulating maps are the only structure that grows with the walk, and they are
bounded by item count rather than by revision count.

## 5. Out of scope

- **Store vs markdown divergence per project** — needs ANTS-4488's read-only
  sync check, which does not exist. Tracked by ANTS-4488.
- **Blocked-by-format counts** (open items missing a kind or layman line) —
  doubles as a pre-flight for ANTS-4483's render gate and belongs with that
  gate. Tracked by ANTS-4483.
- **A per-project breakdown under `scope:"all"`** — a different response shape
  from the flat summed envelope § 2.4 specifies, so it is a second contract
  rather than a field. No id; file one if it is wanted.
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

- `docs/standards/mcp-behavioural-notes.md` — two per-verb notes, not one: a
  `roadmap_query` note for `mode:"report"`, and a `roadmap_log` note for
  `op:"backfill_dates"` recording that it is one-off and walks git.
- `docs/specs/ANTS-3756-roadmap-store-schema.md` — § 7's method census moves,
  and the three date columns stop being write-only. The aggregate SQL lives in
  **one new public `RoadmapStore` reader**, not as raw SQL in the envelope
  builder: every other reader on that surface is a store method, the census
  counts them, and a verb issuing its own SQL against these tables would be the
  second place that knows the schema.
- `docs/specs/ANTS-3765-roadmap-migration-load.md` — INV-3's enumeration names
  `milestone`, `resolution`, `visibility` and `priority` but not the three
  dates, while `Loader::fieldsOf()`'s comment does name them. Widen the
  invariant's list; its claim already covers them.
- `docs/standards/mcp-error-codes.md` — gains `not_a_git_repo` and
  `project_not_registered`. Both are **new**: neither appears in that file
  today, and it is the canonical taxonomy, so a verb minting a code without a
  row there is a refusal nothing else can recognise.
- `CHANGELOG.md` — a user-visible new mode.
- `CLAUDE.md` — no change; the live verb catalogue is `tool_info {catalog:true}`.

## Cold-eyes loop log

| Loop | Date | Lanes | Q-count | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-19 | 3, cold — genre pinned `spec`, cap 2; one byte-stable shared packet carrying the live `item`/`history` schemas, `isWritableItemField()`, `Loader::fieldsOf()`, ANTS-3765 INV-3, the `roadmap_query` mode dispatch and every measurement re-run that morning | **Q1 1 · Q2 3 · Q3 6 · Q4 2** (12 verified / 1 dismissed) | **Twelve verified, twelve fixed; one dismissed as immaterial.** **Three defects were found independently by ALL THREE lanes**, the strongest signal this gate produces, and all three were gaps rather than errors. **The backfill had no surface at all** — § 2.3 specified the walk, its cost, its memory budget and two invariants, and no passage said how it is invoked; INV-10 forbids the report mode from writing, so the one place a reader might have inferred was closed off. Now `roadmap_log op:"backfill_dates"`, with its arguments and two refusal codes, and § 7's behavioural-notes line split in two because the fix made its single-verb routing wrong. **INV-5's test could not fail.** It asked for a write on "a later simulated day" while § 2.2 pinned the date to the local calendar and named no seam — so both writes land on one date and the clause passes against exactly the broken build it targets. § 2.2 now requires an injectable "today" on every write path, modelled on `setRoadmapHistoryCapForTest()`, and INV-5/INV-6 drive it. **`open` was defined twice in one sentence**: the rule said `status != 'shipped'` and the enumeration said planned + in-progress + considered, which disagree about the schema's fifth value `dropped` — and this project has none, so the sample envelope could not disambiguate. Now an enumeration with a stated `totals.items` identity. **The best single-lane finding was a live contradiction between two of my own rules**: § 2.2 clears `shipped` when an item is reopened, so its column is NULL, the never-overwrite rule does not protect it, and git still holds the commit where its marker was ✅ — a backfill reasoning only from git re-closes every reopened item on every run. § 2.3 gained a fourth property and INV-3 a second fixture row. **Two findings were collateral from fixes made earlier the same session**: the `open` sentence and the `by_kind` sample were both written during the author-side pass. **One was found by the orchestrator while verifying, not by a lane** — INV-10 hashed the store file, and the store opens in WAL, so a write lands in the `-wal` sidecar and the hash goes green over a report that wrote rows. Now a row-count and `max(history_id)` check. **Dismissed as true but immaterial:** the 302 ms sample scanned `ROADMAP.md` alone while the walk reads three files, so ~23 s is a lower bound — the command is quoted beside the figure, and nothing built changes. **Three lane open questions resolved clean:** `git rev-list` returns 1496 for `ROADMAP.md` and for `ROADMAP.md docs/roadmap/` alike, so the sample and the population are the same revision set; `putItem()` is public but absent from ANTS-3756 § 7's census because that census counts what ANTS-3765 *added*; and the store's journal mode is `wal`, which became the INV-10 finding above. Doc 382 → 449 lines. |
| 2 | 2026-08-19 | 3, cold — identical brief, packet rebuilt from disk (no source changed between loops), scrubbed copy regenerated so loop 1's row was withheld | **Q1 1 · Q2 2 · Q3 4 · Q4 2** (9 verified / 0 dismissed) | **Nine verified, nine fixed. Cap reached (2 for a spec); the run ships.** **All three lanes independently found the same defect, and it is the most consequential of the whole run** — one loop 1 missed entirely. § 2.2 said `created` is "set at row insert, when `putItem()` creates a row that did not exist", and § 2.2 also said "Migration does not stamp". The migration loader **is** the insert path (`Loader::rebuildElements()` calls `RoadmapStore::putItem()`), so the two rules covered one event and answered it oppositely. One builder dates all 4816 existing rows to migration day — which INV-2's never-overwrite guard would then make permanent — and the other leaves `created` NULL forever, so `added`, an explicitly requested metric, reads 0 in perpetuity. Fixed by exempting the loader in the `created` bullet by name, and by extending the migration paragraph, whose argument reached updates only. **Five of the nine landed on text loop 1 wrote — a 56% collateral share, which is a run doing real work and some oscillating.** The clearest: loop 1's own repair "it skips any id whose current status is not `shipped`" was column-ambiguous, and all three lanes caught it; read as skipping the whole id it leaves every open item undated, killing `added` and `age_open` for exactly the backlog the request is about. Now scoped to the `shipped` column. Loop 1's clock seam said "every **write** path", and the report's own `generated_for` and bucket boundaries are read-path — so INV-1 and INV-8 would have been clock-dependent and flaked at period boundaries. And loop 1's new `scope:"all"` argument had no response shape, leaving the per-project breakdown neither taken nor deferred. **Four were pre-existing draft defects.** `net` was emitted in six places and defined in none, so two builders sign it oppositely. INV-8's clause asked a fixture to sum "the two month buckets" when the envelope emits one — unfalsifiable as written; now two `since`/`until` windows, which is also why `since` gained a paired bound and a stated half-open rule. **The Q1 was a figure I dismissed in loop 1 as immaterial and three lanes queried across both loops** — the 302 ms sample read one file where the walk reads three. Re-measured properly: 228 ms for 20 revisions over all three, so the walk is **17 s, not 23 s**, and § 4's OOM sentence carried the stale figure too. Dismissing it was the wrong call; a number three cold readers stumble on is not inert. **One fix was caught by 4a step 3, not by a lane:** the `created` repair cited `Loader::insertNew()`, which does not exist — the caller is `Loader::rebuildElements()`. Executing the claim is what found it. **Also settled:** `not_a_git_repo` and `project_not_registered` appear nowhere in `mcp-error-codes.md`, so § 7 now names that file as one this spec amends. Doc 449 → 494 lines. |
