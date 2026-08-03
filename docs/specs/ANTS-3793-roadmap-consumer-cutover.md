# ANTS-3793 — roadmap consumer cutover: one reader seam, two backends

**Status:** spec draft (2026-08-03).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3793 (ANTS-3758 split, spec seam 3b of 5); the
`item.body` half from ANTS-3808, found while verifying ANTS-3806 (2026-08-03)
and homed here by the user the same day.
**Covers:** ANTS-3793, ANTS-3808.
**Blocked by:** ANTS-3758 (the render these consumers write through) — shipped.
**Blocker for:** ANTS-3794 (publish + health checks), which would otherwise
publish § 2.3's duplication into every migrated project's `ROADMAP.md`.
**Pairs with:** ANTS-3765 (the load half, whose § 2.10 marker § 2.2 consumes),
ANTS-3757 (the read half, whose § 2.1.1 § 2.3 amends).

**Contents:** [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 The seam](#21-the-seam-one-reader-function-two-backends) ·
[2.2 The dispatch marker](#22-the-dispatch-marker) ·
[2.3 What `item.body` holds](#23-what-itembody-holds-ants-3808) ·
[2.4 The write half](#24-the-write-half--roadmap_logs-eight-ops) ·
[2.5 RoadmapDialog](#25-roadmapdialog-and-the-legend) ·
[2.6 The round-trip oracle](#26-the-round-trip-oracle-inv-1s-deferred-half) ·
[2.7 Acyclicity](#27-whole-store-relationship-acyclicity)) ·
[3. Invariants](#3-invariants) · [4. RAM / build cost](#4-ram--build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

The store is primary after cutover (`roadmap-data-model.md` INV-3) and
ANTS-3758 can now write markdown back out of it, but **nothing reads or writes
the store yet**. Every consumer still parses `ROADMAP.md`: `roadmap_query`,
`roadmap_log`'s eight ops, and `RoadmapDialog`. Until they move, the store is a
write-only copy that drifts from the file the moment anyone edits it.

Two further things block on this id, both because it owns the seam they
straddle:

- **ANTS-3808** — the migration files `BulletRecord::body` (the *whole* bullet)
  into `item.body`, and `RoadmapRender::renderBullet()` reads that column as
  *residual prose*. Two contracts, one column, opposite meanings, so a rendered
  bullet repeats its own headline and then every field a second time. Observed
  in the ANTS-3806 fixture, not inferred.
- **ANTS-3758's INV-1 full oracle**, deferred at ship because it needs the
  migration loader and a second store. This id has both in hand, and the oracle
  is what would have caught ANTS-3808.

**The measurement that shapes this spec.** The consumer surface reads as
enormous — `grep -o 'roadmap_log' src/remotecontrol.cpp | wc -l` returns 205
(2026-08-03) — but those are mentions, not read paths. Every consumer read goes
through **one function**:

```
$ grep -n 'parseBullets(' src/remotecontrol.cpp src/roadmapdialog.cpp \
    | grep -v 'roadmapdialog.cpp:51[489]:' | wc -l
26
```

All 26 call `RoadmapDialog::parseBullets()`, which since ANTS-3764 is a
forwarder whose entire body is `return RoadmapParse::parseBullets(markdownText);`
(`src/roadmapdialog.cpp`). The excluded lines are that forwarder's own comment,
signature and body. `src/roadmapmigrate.cpp`'s call is
**not** a consumer — it is the migration reading source markdown to build the
store, and it stays markdown forever.

So the read cutover is not 26 sites. It is one function with two backends, and
the 26 call sites plus the 82 under `tests/` are untouched:

```
$ grep -rn 'parseBullets(' tests/ --include=*.cpp | wc -l
82
```

## 2. Surface

### 2.1 The seam: one reader function, two backends

`RoadmapParse::parseBullets(markdownText)` is a pure text-to-records function
and stays exactly that. The cutover adds a **sibling producer of the same
record type**, sourced from the store, and a resolver that picks between them:

```cpp
// src/roadmapsource.h — new TU in ants_roadmapstore_lib.
namespace RoadmapSource {
    // The records a consumer would have got from parsing this project's
    // markdown, sourced from the store instead. Document order, one entry per
    // renderable item, in the SAME shape RoadmapParse::parseBullets returns.
    std::optional<QVector<BulletRecord>>
    bulletsFromStore(RoadmapStore &store, qint64 projectId, QString *error);

    // § 2.2's dispatch. nullopt = this project is not migrated; the caller
    // parses markdown as it does today.
    std::optional<qint64> migratedProject(RoadmapStore &store,
                                          const QString &projectRoot);
}
```

**`BulletRecord` is the interface, and that is a deliberate constraint rather
than a convenience.** Every consumer, every response field and all 82 test call
sites are already written against it, so a store backend that fills the same
struct changes no caller and no test. It also makes the two backends directly
comparable, which is what INV-2 asserts and what no wider refit would allow.

**Fields the store cannot fill are left at their defaults, and that set is
enumerated rather than discovered.** `sourceStatus`, `passDesignator`, `anchor`,
`boldId`, `synthetic` and `idToken` are all artefacts of *which markdown dialect
was parsed* (`src/roadmapparse.h`'s own comments say so for each), and the store
holds normalised status and ids instead. A consumer that needs one of them is a
consumer that needs the markdown path, which is § 5's boundary and INV-3's
subject.

### 2.2 The dispatch marker

ANTS-3765 § 2.10 already fixed the marker and its guarantee: **a `project` row
exists exactly when that project's whole plan committed**, because per-project
atomicity makes "half a project" unreachable. That spec states the marker and
explicitly does not build the fallback. This one builds it.

`migratedProject()` resolves the caller's project root through
`RoadmapStore::readProjectBySlug()` and returns its `projectId`, or `nullopt`.
Three rules follow, and each exists because the obvious reading is wrong:

- **The store not existing is `nullopt`, not an error.** Most machines running
  this code have never migrated anything, and a verb that refused there would
  break every unmigrated project on the day the store shipped.
- **The store existing and failing to open IS an error**, surfaced with the
  `mcp-error-codes.md` taxonomy and never silently downgraded to the markdown
  path. A corrupted store that quietly falls back is a store nobody notices is
  corrupt — the same failure class `roadmap-data-model.md` § 9 names for silent
  backup loss.
- **The connection profile is `Access::Bulk` for a migration and
  `Access::Interactive` for a consumer.** An Interactive connection is refused
  for bulk work and the refusal does not read like a profile problem, so the
  profile is named at each call site rather than defaulted.

**The dispatch is resolved once per call, not once per process.** A verb call
that resolved the marker at startup would serve markdown for the rest of the
session to a project migrated in between.

### 2.3 What `item.body` holds (ANTS-3808)

**The disagreement, stated exactly.** `RoadmapParse::parseBullets()` seeds
`QString body = head` where `head` is the bullet line minus its `"- "` and minus
its status emoji (`stripInlineEmoji()` runs first on the native path), then
appends every continuation line trimmed of indentation. So `body`'s first line is
the id-and-headline text and the rest is every continuation line, the
`Layman:` / `Kind:` / `Source:` / `Lanes:` / `Evidence:` trailer included.
`RoadmapMigrate`'s `makeItem()` copies that verbatim into `item.body`, which is
what ANTS-3757 § 2.1.1 tells it to do. `RoadmapRender::renderBullet()` then emits
a synthesised head line, **then** `body`, **then** the trailer again from the
columns. Three copies of the headline-and-fields for one bullet.

**Why the obvious repair is wrong.** "Store `body` minus its head line minus its
trailer lines" assumes the trailer *is* a set of lines. `src/roadmapparse.cpp`'s
own regex comments carry the corpus measurements that say otherwise: 157
`Source:` values sit inline in a prose sentence rather than at a line start
(which is why `rxSource` and `rxLanes` are deliberately un-anchored, ANTS-2058 /
ANTS-3764), 10 lines carry two keys (`rxTrailerKey` exists for exactly those),
and 22 are backticked mentions of a key excluded by ANTS-3722's guard. For a
large share of the corpus there is no field *line* to drop — the metadata is a
span inside a sentence, and excising it either deletes prose or leaves half a
sentence. Computing the residual is a new parsing contract, and re-deriving it
outside `RoadmapParse` is the second bullet parser ANTS-3757 § 2.3 forbids.

**The decision: the migration drops one line, and the render asks before it
re-derives.** Two changes, neither of which needs a schema change, a new
provenance value, or a second parser.

1. **`item.body` is the bullet body with its first line removed.** This half is
   well-defined and needs no parsing at all: `body` is seeded from `head` and
   every continuation is appended after a `'\n'`, so the head line is exactly
   the text before the first `'\n'` and the residual is exactly the text after
   it — empty for a bullet with no continuation. ANTS-3757 § 2.1.1's row for
   `body` is amended from "the reader's `body`" to say so.

2. **`renderBullet()` emits a trailer key from its column only when the body
   does not already carry that key**, asked through `RoadmapParse`'s own
   matchers. The five regexes move from function-static locals to one exported
   predicate:

   ```cpp
   // src/roadmapparse.h — the matchers, asked rather than duplicated.
   struct TrailerKeys { bool layman, kind, source, lanes, evidence; };
   TrailerKeys trailerKeysIn(const QString &body);
   ```

   This is **not** a second parser. It is the one reader answering a question
   about its own grammar, which is the distinction ANTS-3757 § 2.3 draws.

**Why per-key and not a stored flag.** A `verbatim`-versus-`residual` flag on
`provenance.body` was the sketch on the ANTS-3808 bullet, and it is wrong for a
reason worth recording: `provenance` is per-field **write origin** —
`migrated` / `asserted` / `defaulted` (`src/roadmapstore.h`, the `setItemField`
overload comment) — and body *shape* is a different axis. A migrated body edited
by a consumer flips its provenance to `asserted` while staying verbatim, and a
post-cutover body written residual would carry `asserted` too. One key cannot
answer both questions, and the per-key predicate answers the question actually
being asked without storing anything.

**The corner this design has, named rather than papered over.** A body whose
prose mentions a trailer key un-backticked ("the Kind: field decides…")
suppresses that key's rendered line. That is not a new defect: the *reader*
already extracts `kind` from exactly that sentence, so the store and the render
agree, and INV-1's round-trip holds. It is inherent to the un-anchored regexes
ANTS-2058 chose deliberately, and the alternative — anchoring them — would lose
the 157 inline values that motivated un-anchoring. § 5 records it as a known
limitation of the format rather than of this spec, and INV-6 pins the
consistency that makes it survivable.

### 2.4 The write half — `roadmap_log`'s eight ops

The dispatcher accepts seven named ops plus `append` as the unnamed fallthrough
(`src/remotecontrol.cpp`, `RemoteControl::cmdRoadmapLog`):

```
$ awk '/^QJsonDocument RemoteControl::cmdRoadmapLog\(/,/^}/' src/remotecontrol.cpp \
    | grep -o 'op == QStringLiteral("[a-z_]*")' | sed 's/.*("//;s/")//' | sort -u
amend_body annotate append_batch bundle_row create_section flip flip_batch
```

Each hand-splices markdown and commits it with `QSaveFile` — ten such writes on
a roadmap path (`grep -c 'QSaveFile [a-z]*(roadmapPath)' src/remotecontrol.cpp`
→ 10, 2026-08-03).

**On a migrated project every op becomes: mutate the store, then re-render.**
The render already exists and is already proved idempotent and lossless
(ANTS-3758 INV-1 / INV-7), so no op grows a markdown writer of its own. The
splice paths stay on the unmigrated branch, unchanged, and are deleted by the
id that retires markdown — not this one.

**Two ops do not map onto item rows, and pretending they do is how this half
goes wrong.** `create_section` writes a `section` row (`addSection()`), which the
store models directly. `bundle_row` appends a Markdown *table* row — a
`kind = 'table'` element in the store's element model, whose payload is the
rendered row. Both are element-level writes, not item writes, and § 2.7 of the
data model already carries the element kinds.

**`annotate` and `flip` share one path today and must keep sharing one**
(`cmdRoadmapLog` routes both to `cmdRoadmapLogFlip`). On the store side that is
`setItemField()` for the body append plus, for `flip` only, a `status` write —
so the shared path is preserved rather than reinvented, and `annotate` remains
the flip that changes no status.

**Every store write is one transaction, and a failed render rolls it back.** A
committed store change whose render failed leaves the file disagreeing with the
store, which is the divergence ANTS-3794 exists to detect and must never be
caused here. INV-4 pins it.

### 2.5 RoadmapDialog, and the legend

The dialog's three consumer reads go through the same § 2.1 resolver and need no
other change: it renders `BulletRecord`s and will receive `BulletRecord`s. They
sit in `RoadmapDialog::renderCardsHtml()`, `captureScrollAnchor()` and
`restoreScrollAnchor()` — and the latter two matter more than their names
suggest, because each re-parses the *whole* file to locate one anchor. On the
store path they become a store read per scroll restore, which is why § 4 bounds
`bulletsFromStore()` by item count and why INV-2's record-for-record equality is
what keeps the anchor landing in the same place.

**The dialog renders each project's own legend when the store has one, and no
legend when it does not.** ANTS-3793's bullet leaves this open — the data
model's § 5.1 "makes it possible, not mandatory". Deciding it *renders* costs
one `readProject()` the dialog already needs for the project row, and the
alternative silently shows this project's vocabulary for another project's
statuses. A project with no stored legend shows none rather than a default, for
the reason ANTS-3758 § 2.8 gives: the legend is per project precisely so one
renderer serves every project's vocabulary.

### 2.6 The round-trip oracle: INV-1's deferred half

ANTS-3758 shipped INV-1's standalone half — every field survives into the text —
and deferred the full render → load → export comparison here, because it needs
`RoadmapMigrateLoad`, a second store and its `Options`. § 2.6 of that spec
already fixes the whole contract: render to a scratch project root, rediscover
it with `findRoadmaps()`, load it into a scratch store, export both, and compare
**projections taken with the same predicate**, with three enumerated families
excluded. This spec builds it and adds nothing to the contract.

**It is built before § 2.3's fix and shown red against it.** ANTS-3808 is
exactly the defect this oracle exists to catch, and a fixture that only ever
runs against corrected code proves the oracle compiles, not that it discriminates.

### 2.7 Whole-store relationship acyclicity

Deferred here by ANTS-3760 finding 9, and distinct from the per-row
self-relationship `CHECK` the schema already enforces: that constraint stops
`A → A` and cannot see `A → B → A`.

**It is a check, not a constraint.** SQLite cannot express a graph-reachability
constraint in DDL, and enforcing acyclicity on every `relateItems()` call would
put a traversal in the write path of the migration's hottest loop. It runs as a
health check over the finished store — the same family ANTS-3794 schedules — and
**reports** rather than refuses, because a cycle is a data fault to surface, not
a write to reject after the fact.

## 3. Invariants

- **INV-1** — **A migrated project's rendered bullet contains its headline once
  and each trailer key once.** *Breaks when:* the migration stores the head line
  in `item.body` (today's defect), or `renderBullet()` emits a column-sourced
  trailer line for a key the body already carries. *Test:*
  `roadmap_consumer_cutover/` case `Inv1NoDuplication`, which renders the
  ANTS-3806 fixture's `DEMO-0003` and asserts one occurrence of the headline and
  one `Kind:`.
- **INV-2** — **Both backends produce the same `BulletRecord`s for the same
  project**, over the fields § 2.1 says the store fills. *Breaks when:* the store
  backend orders by `id` rather than document order, drops narration elements'
  effect on position, or fills a dialect-only field with a fabricated value.
  *Test:* `Inv2BackendsAgree`, which parses a fixture's markdown, migrates the
  same markdown, and compares record-for-record.
- **INV-3** — **An unmigrated project is served by the markdown path, and a
  migrated one never is.** *Breaks when:* the marker is resolved once per process
  rather than per call, or a store that fails to open falls back silently.
  *Test:* `Inv3DispatchMarker`, which queries a project before and after loading
  it into the store within one process.
- **INV-4** — **A `roadmap_log` op that fails to render leaves the store
  unchanged.** *Breaks when:* the store transaction commits before the render
  runs. *Test:* `Inv4WriteRollsBack`, which fails the render and asserts the
  item's pre-op field values.
- **INV-5** — **`RoadmapParse` remains the only bullet grammar in `src/`.**
  *Breaks when:* the render or a consumer grows its own `Kind:` / `Source:`
  matcher instead of calling `trailerKeysIn()`. *Test:* `Inv5SingleGrammar`, a
  case-sensitive source scrape for the trailer-key regex literals outside
  `src/roadmapparse.cpp`, comments stripped — the shape ANTS-3758's INV-11 had to
  be corrected into after matching English prose.
- **INV-6** — **The render and the reader agree about every trailer key.**
  Re-parsing a rendered bullet yields the same `kind` / `source` / `lanes` /
  `layman` / `evidence` the store holds, including for a body whose prose
  mentions a key. *Breaks when:* the render suppresses a key the reader would not
  have extracted, or emits one the reader then reads twice. *Test:*
  `Inv6RenderReaderAgree`, whose fixture includes § 2.3's un-backticked prose
  mention.
- **INV-7** — **The full round-trip loses nothing and invents nothing.**
  ANTS-3758 INV-1's deferred half: render → `findRoadmaps()` → `planFrom()` →
  `load()` → export, compared against the source store's export under § 2.6's
  projection. *Breaks when:* any non-defaultable field is dropped from the
  bullet, or an element is emitted out of order. *Test:* `Inv7RoundTrip`, shown
  **red against the pre-§ 2.3 migration** before the fix is applied.
- **INV-8** — **A relationship cycle is reported, not refused and not ignored.**
  *Breaks when:* the check reports only self-relationships (which the `CHECK`
  already covers), or `relateItems()` starts rejecting writes. *Test:*
  `Inv8Acyclicity`, which stores `A → B → A` and asserts both the write
  succeeding and the check naming the cycle.

## 4. RAM / build cost

**RAM.** `bulletsFromStore()` materialises one project's records — the same
`QVector<BulletRecord>` the markdown path already materialises, from the same
data, so peak is unchanged for every existing caller. It is bounded by item
count, not by store size: a project's items, not the corpus. The oracle (§ 2.6)
holds two stores and two exports at once and is a test-only path, which is where
ANTS-3758 § 2.6 already put that cost.

**Build.** One new TU (`roadmapsource.cpp`) in `ants_roadmapstore_lib`, plus one
exported predicate in the existing `roadmapparse.cpp`. No new library, no new
link edge: `remotecontrol.cpp` and `roadmapdialog.cpp` already link the store lib
through the render. Per this project's cap, builds run under `cmake --build
build` with the `JOB_POOLS` limit and tests at `ctest -j4`.

## 5. Out of scope

- **Deleting the markdown splice paths.** They serve every unmigrated project
  for as long as the rollout takes (§ 2.2), and the id that retires them is not
  this one.
- **Non-emoji formats.** The store backend serves `ants-v1` emoji bullets only.
  3D_Engine (GFM task lists) and RetroDB (pass headings) keep the markdown path,
  which is the same scoping decision ANTS-3758's render made and for the same
  reason.
- **The un-anchored-key corner (§ 2.3).** A body mentioning a trailer key
  un-backticked in prose is mis-read by the *reader* today; this spec keeps the
  render consistent with it (INV-6) rather than fixing the grammar. Fixing it
  means anchoring `rxSource` / `rxLanes`, which would lose the 157 inline values
  ANTS-2058 un-anchored them for — a format decision, needing its own id.
- **The auto-publish cadence and the remaining health checks** — ANTS-3794,
  which § 2.7's check is scheduled by.
- **`roadmap-format.md` § 3.5.1's counter definition**, inherited as an
  obligation from ANTS-3758 § 7 and discharged in § 7 below rather than as
  surface here.

## 6. Tests

`tests/features/roadmap_consumer_cutover/`, label `features`, compiled into an
existing bundle per `tests/features/README.md` (no `add_executable`).

| Case | Invariants |
|---|---|
| `Inv1NoDuplication` | INV-1 |
| `Inv2BackendsAgree` | INV-2 |
| `Inv3DispatchMarker` | INV-3 |
| `Inv4WriteRollsBack` | INV-4 |
| `Inv5SingleGrammar` | INV-5 |
| `Inv6RenderReaderAgree` | INV-6 |
| `Inv7RoundTrip` | INV-7 |
| `Inv8Acyclicity` | INV-8 |

Per this project's convention, **every case is verified RED against its *Breaks
when* mutation before the implementation is restored.** Where that is scripted,
files are restored with `write_text` and never `shutil.copy2` — `copy2` preserves
mtime, ninja then skips the rebuild, and the mutation accumulates silently in a
binary that still links green.

Two fixture rules this spec's cases inherit from ANTS-3758's implementation, both
learned by a case failing rather than by review:

- **Never default-construct `RoadmapStore`.** It resolves `defaultPath()` — the
  developer's real store under `XDG_DATA_HOME` — and every case would write into
  it. Always `std::make_unique<RoadmapStore>(dir.filePath("store.db"))`.
- **`ctest -R` is case-sensitive.** Run `ctest -N -R <pattern>` and check the
  match count before trusting a green run.

## 7. Cross-doc impact

- **ANTS-3757 § 2.1.1's `body` row** reads "the reader's `headline` / `body`".
  § 2.3 changes what is stored, so the row is amended on ship to say the head
  line is dropped — otherwise the migration's own spec describes the defect.
- **ANTS-3765 § 2.10** says the fallback is "ANTS-3758's, not this half's".
  ANTS-3758 did not build it; § 2.2 does. That sentence is amended to name this
  id.
- **ANTS-3758 § 2.4's INV-1** carries the deferral to this id; on ship its
  invariant gains this spec's `Inv7RoundTrip` as the full-oracle surface, and its
  loop-log row 4-impl clause (3) is annotated as discharged.
- **`roadmap-format.md` § 3.5.1's counter definition** still needs its cutover
  amendment — inherited from ANTS-3758 § 7, and this is the id that owns it. On a
  migrated project the id high-water lives in the store
  (`RoadmapStore::idHighWater()`), not in `.roadmap-counter`, and the standard
  must say which is authoritative during the interim.
- **`roadmap-data-model.md`'s *What checks this* table** gains INV-7 against the
  round-trip row and INV-8 against relationship acyclicity, which ANTS-3760
  finding 9 left with no owner.
- **`CLAUDE.md`'s module map and `docs/subsystems.md`** gain `roadmapsource`.
- **ANTS-3808's ROADMAP bullet** is closed by this spec's ship, not by a separate
  change — its own body records that homing.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
