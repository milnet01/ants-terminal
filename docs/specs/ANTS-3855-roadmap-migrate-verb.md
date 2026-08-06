# ANTS-3855 — `roadmap_migrate`: the production entry point that loads a project into the store

**Status:** spec draft (2026-08-06).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3855 (in-session-2026-08-06, measured while starting ANTS-3853's first item).
**Blocker for:** ANTS-3807 (per-project migration briefs), ANTS-3772, ANTS-3815.
**Composes with:** ANTS-3757 (read half), ANTS-3765 (load half), ANTS-3793 (consumer cutover).

**Layman:** The roadmap database is built and tested but nothing can put a
project into it. This adds the command that does — with a preview mode that
reports what it would do before it writes anything.

## 1. Problem

The roadmap store is complete and unreachable. Schema (ANTS-3756), read half
(ANTS-3757), load half (ANTS-3765), archives (ANTS-3766), render (ANTS-3758),
export (ANTS-3761), section provenance (ANTS-3782), consumer cutover
(ANTS-3793) and write half (ANTS-3809) have all shipped. **Nothing in
production calls any of them.**

Three entry points, zero non-test call sites, measured 2026-08-06:

```console
$ rg -n 'RoadmapMigrateLoad::load\s*\(' src/ | grep -v ': *//'
NONE
$ rg -n 'RoadmapMigrate::(findRoadmaps|planFrom)\s*\(' src/ | grep -v ': *//'
NONE
```

The only two `src/` mentions of `RoadmapMigrateLoad::load` are prose in
`src/roadmapmigrate.h` and `src/roadmapstore.h`. Every invoker is a test under
`tests/features/`.

Four consequences, and they are why this is the migration programme's first
item rather than a convenience:

1. **No project can be cut over.** The live store on this machine holds one
   leaked fixture row (`root = /tmp/test_core-ZnzBrv`, 0 sections, 0 items —
   ANTS-3856) and no real project. Every live read and write is still the
   markdown path.
2. **The cross-project rollout cannot start.** ANTS-3807 plans to hand each
   project's CC session a brief, but names no thing that session would invoke.
   ANTS-3772's per-project id collisions cannot be re-measured against a real
   run either.
3. **ANTS-3815's payoff is unobservable.** On an unmigrated project
   `RoadmapSource::migratedProject()` returns `nullopt` at the
   `readProjectByRoot()` miss and the source-format gate never runs at all.
4. **The one-standard goal has no delivery mechanism**, which is what
   ANTS-1160's dialog redesign is waiting on.

## 2. Surface

### 2.1 The verb

`roadmap_migrate` — one project per call, resolved from `caller_cwd`.
Handler `RemoteControl::cmdRoadmapMigrate` in a new TU
`src/remotecontrol_roadmap_migrate.cpp`, joining `ants_core_lib`'s `SOURCES`
beside its two siblings (`remotecontrol_roadmap_query.cpp`,
`remotecontrol_roadmap_log.cpp`). **No new link edge**: `ants_core_lib`
already links `ants_roadmapstore_lib` PRIVATE (`CMakeLists.txt`, the
ANTS-3793 § 4 comment at the `PRIVATE ants_roadmapstore_lib` line), so both
migration namespaces are reachable from core today.

`CallerCwdContract::Required` — a migration is a whole-project operation and
has nothing to fall back to when no project is named.

| Arg | Type | Default | Meaning |
|---|---|---|---|
| `caller_cwd` | string | — | Required. The project root, per ANTS-1401. |
| `dry_run` | bool | `false` | Plan every write, report the counts, roll back. |
| `project_name` | string | leaf dir of the canonical root, verbatim | `project.name`. |
| `export_slug` | string | slugified leaf dir | `project.export_slug`. |

`project_name` and `export_slug` are arguments because **nothing derives
them**: `RoadmapMigrate::planFrom()` takes both from its caller by design
(`src/roadmapmigrate.h` — "`projectName` and `exportSlug` are supplied by the
caller, not derived … whose charset the store constrains and which nothing in
a markdown file carries"). The defaults follow `roadmap_log`'s existing
precedent of deriving from `caller_cwd`'s leaf directory rather than asking.

**Slugification** lowercases, replaces every run of non-`[a-z0-9]` with a
single `-`, and strips leading/trailing `-`. `Ants_Terminal` → `ants-terminal`.

### 2.2 The connection is the verb's own, and it is `Access::Bulk`

The handler constructs and opens its **own** `RoadmapStore` at
`RoadmapStore::defaultPath()` on `Access::Bulk`, for the duration of one call.
It must **not** reuse `RemoteControl::roadmapStoreOrNull()`, which is the
process-owned `Access::Interactive` connection ANTS-3793 § 2.2 opened for the
consumers: `RoadmapMigrateLoad::load()` refuses a non-`Bulk` store outright
(`src/roadmapmigrateload.cpp` — "a migration load needs an `Access::Bulk`
store", ANTS-3765 INV-12).

Two live connections in one process are safe by construction, not by luck:
`RoadmapStore`'s constructor allocates a per-instance connection name from an
atomic counter (`src/roadmapstore.cpp` — "One QSqlDatabase connection name per
instance: two stores in one process … must not share a connection"), and the
store runs in WAL.

**The consumers pick the migration up without a restart, and this is already
guaranteed rather than added here.** `roadmapStoreOrNull()` states that
"absence is NOT remembered: a store that does not exist yet costs one stat per
call to re-check, and remembering it would serve markdown for the rest of the
session to a project migrated in between." So a first-ever migration —
which *creates* the store file — is visible to the very next `roadmap_query`
in the same session. INV-8 locks it.

### 2.3 The sequence, and where each refusal falls

```cpp
// src/remotecontrol_roadmap_migrate.cpp — the whole verb.
1. root = ants::resolveCallerCwdRoot(...)        // empty → caller_cwd_required
2. slug = export_slug arg, else slugify(leaf)    // invalid → bad_args, NOTHING opened
3. disc = RoadmapMigrate::findRoadmaps(root, &code)   // nullopt → refuse on `code`
4. plan = RoadmapMigrate::planFrom(disc, name, slug)  // pure; cannot fail
5. store.open()                                  // fails → store_failed
6. opts = { changedAt: <one stamp>, projectRoot: root, dryRun: <arg> }
7. out  = RoadmapMigrateLoad::load(store, plan, opts)
8. envelope from `out`                           // out.ok false → migrate_failed
```

**Step 2 precedes step 5 deliberately.** An invalid slug that reached
`registerProject()` would fail the DDL's `export_slug` CHECK *inside* the
transaction and roll back a whole migration, reporting a store error for what
is an argument error. Validating first refuses in microseconds and leaves the
store untouched (INV-6).

**The stamp is taken once, at step 6** —
`QDateTime::currentDateTimeUtc().toString(Qt::ISODate)`, which yields the exact
shape `history.changed_at` CHECKs and `isIsoZStamp()` validates
(`^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`, `src/roadmapmigrateload.cpp`).
`Options::changedAt` is a parameter and not a clock read precisely so a run is
reproducible (ANTS-3765 § 2.1); a handler that stamped per row would defeat
that from the outside.

### 2.4 Response

```json
{
  "ok": true,
  "dry_run": false,
  "project_id": 2,
  "project_name": "Ants_Terminal",
  "export_slug": "ants-terminal",
  "store_path": "/home/…/.local/share/ants-terminal/roadmap.sqlite",
  "changed_at": "2026-08-06T21:14:07Z",
  "sources": [{"path": "ROADMAP.md", "format": "ants-v1"}],
  "items_inserted": 0, "items_updated": 0, "items_unchanged": 0,
  "items_orphaned": 0, "ids_allocated": 0,
  "sections_written": 0, "elements_written": 0, "history_rows": 0,
  "notes": [{"code": "…", "detail": "…", "line": 0, "source_index": 0}],
  "notes_count": 1
}
```

The counts above are the **shape**, not a measurement — every value is
zeroed so no figure here can be read as this project's. § 4 carries the
measured ones, with the run that produced them.

Every count is `RoadmapMigrateLoad::Outcome`'s corresponding field, renamed to
the envelope's snake_case and **not** recomputed — the `Outcome` is "a value,
not a log … every outcome assertable by a test" (`src/roadmapmigrateload.h`),
and a second tally would be a second answer.

`notes[]` is `Outcome::notes` in order, one object per
`RoadmapMigrate::Note`, carrying `source_index` verbatim including its `-1`
sentinel (ANTS-3766 § 2.4). It is never filtered: the load's notes are already
a superset of the plan's, so one envelope covers the whole migration.

`sources[]` is `plan.sources` reduced to `{path, format}` — `markdown` is
deliberately dropped, being the multi-megabyte input the caller already has.

### 2.5 Refusals

| Condition | `code` |
|---|---|
| No `caller_cwd` | `caller_cwd_required` |
| Root does not resolve | `no_project` |
| `export_slug` fails the store's charset | `bad_args` |
| `findRoadmaps()` → `not_found` | `no_roadmap` |
| `findRoadmaps()` → `case_ambiguous` | `case_ambiguous` |
| `findRoadmaps()` → `not_utf8` | `not_utf8` |
| `findRoadmaps()` → `archive_format_mismatch` | `format_mismatch` |
| `store.open()` fails | `store_failed` |
| `load()` returns `ok == false` | `migrate_failed`, `error` = `Outcome::error`, `notes` carried |

`no_roadmap`, `format_mismatch`, `bad_args`, `store_failed`, `no_project` and
`caller_cwd_required` are already canonical
(`docs/standards/mcp-error-codes.md`). `case_ambiguous`, `not_utf8` and
`migrate_failed` are new and are added to that taxonomy in the same change
(§ 7) rather than folded into a near neighbour — a project refused because two
roadmap files differ only in case needs a different next step from one refused
for invalid bytes, and collapsing them loses exactly the distinction the
operator acts on.

**A refusal writes nothing.** Steps 1–4 precede any store open; step 7 is one
transaction per project, so `Outcome::ok == false` means "NOTHING for this
project was committed" (`src/roadmapmigrateload.h`). INV-4 locks it.

## 3. Invariants

- **INV-1** — A non-test translation unit calls all three migration entry
  points; the verb is the only one that does. *Test:*
  `rg -n 'RoadmapMigrateLoad::load\s*\(' src/ | grep -v ': *//'` names
  `src/remotecontrol_roadmap_migrate.cpp` and no other file; same for
  `RoadmapMigrate::(findRoadmaps|planFrom)`. Source-grep in the feature test.
- **INV-2** — The handler migrates on its own `Access::Bulk` connection and
  never the process-owned `Access::Interactive` one. *Test:* the feature test
  drives the handler against a fresh store and asserts `ok:true` — an
  `Interactive` connection is refused by `load()`'s INV-12 check, so the two
  outcomes differ.
- **INV-3** — `dry_run:true` commits nothing and reports the counts the real
  run produces. *Test:* feature test — dry run, assert `project` has no row
  for this root; then the real run, assert every count field equals the dry
  run's (ANTS-3765 INV-13's comparison, driven through the verb).
- **INV-4** — Every refusal in § 2.5 leaves the store byte-unchanged. *Test:*
  feature test — hash the store file, drive each refusal, re-hash.
- **INV-5** — One stamp per call, in the shape `history.changed_at` CHECKs.
  *Test:* the response's `changed_at` matches
  `^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`, and every `history` row written by
  one call carries that identical value.
- **INV-6** — An invalid `export_slug` refuses `bad_args` before the store is
  opened. *Test:* feature test — `export_slug: "Ants_Terminal"` on a machine
  with **no** store file; assert `bad_args` and that no store file was created.
- **INV-7** — A second run over an unchanged project is idempotent:
  `items_inserted == items_updated == items_orphaned == ids_allocated == 0`,
  and the project still has exactly one row. *Test:* feature test runs the
  verb twice over one fixture root.
- **INV-8** — After a successful run the consumer path serves that project
  from the store in the same process, with no restart. *Test:* feature test —
  `roadmapStoreServes(root, markdown)` is false before and true after.

## 4. RAM / build cost

**Memory.** One `Access::Bulk` connection's **16 MiB** page cache
(`kBulkCacheKiB`, ANTS-3756 § 2.5) plus one plan, for the duration of one
call, then released — the verb opens and closes per call because a migration
is rare, which is the opposite trade from ANTS-3793's process-owned
`Interactive` connection and correct for the same reason. ANTS-3757 § 4
budgets the plan itself; nothing here holds state between calls.

**Time.** Measured 2026-08-01 by ANTS-3765 § 4 (quiet machine, two runs
agreeing within 6 ms) over the ten real project roots under
`/mnt/Games/Scripts/Linux/`: the worst project is **Ants_Terminal at 129 ms**
(1,794 items / 1,888 elements, 2.9 MB of source), re-run 134 ms. That is
inside the MCP bridge's 60 s call timeout (ANTS-3444) by more than two orders
of magnitude, so the verb is synchronous — no job/poll surface, and nothing
here justifies one.

**Build.** One new `.cpp` in an existing library. No new target, no new
dependency, no new link edge (§ 2.1). The feature test joins `test_core`'s
`SOURCES` per `tests/features/README.md`.

## 5. Out of scope

- **A dialog action.** Deferred; the MCP verb is the surface ANTS-3807's
  per-project briefs invoke, and a button would call this handler anyway. No
  id filed — file one when a user wants it, rather than promising work nobody
  has asked for.
- **Un-migrating.** A permanent exclusion. The store's documented rebuild path
  is the export (ANTS-3761, `src/roadmapstore.h`); a bespoke undo would be a
  second one that has to stay correct forever.
- **Migrating N projects in one call.** A permanent exclusion:
  `RoadmapMigrateLoad::load()` is "one plan, one project, one transaction"
  (ANTS-3765 § 2.5), and a multi-project verb would either break that
  atomicity or hold one write lock across every project.
- **The source-format column** — ANTS-3815. **The batched item reader** —
  ANTS-3816. **The schema-upgrade path** — ANTS-3781, which this spec does not
  need because it adds no column.
- **Fixing the corpus's id collisions** — ANTS-3772. This verb reports them as
  a refusal with the plan's notes; it does not repair source files.
- **Evicting the leaked fixture row** — ANTS-3856.

## 6. Tests

Feature test: `tests/features/roadmap_migrate_verb/`. Covers INV-1..INV-8.
Label `features;fast`. Source added to `test_core`'s `SOURCES` list — not
`add_executable` (`tests/features/README.md`).

Each invariant is verified to FAIL against pre-change source before the
handler is restored, per the project test convention. INV-1 fails trivially
(no such TU); INV-2..INV-8 are driven through the registered handler and fail
at dispatch until it exists.

Fixture: a `QTemporaryDir` project root carrying a small hand-written
`ants-v1` `ROADMAP.md`, and a store at an **explicit** path inside that temp
dir — never `RoadmapStore::defaultPath()`, which is the real store and is how
ANTS-3856's leaked row got there. The handler resolves `defaultPath()`
internally, so the test drives the seam functions the handler composes and
asserts the handler's own path resolution by source-grep.

## 7. Cross-doc impact

- **`docs/standards/mcp-error-codes.md`** gains `case_ambiguous`, `not_utf8`
  and `migrate_failed` (§ 2.5).
- **`docs/standards/mcp-tools.md`** — no change to the standard, but the
  authoring checklist is walked in full for this verb: `tools/list` schema
  entry, `callerCwdContractFor` → `Required`, `kindForName` bucketing,
  `tokenCostFor`, and `registerToolProvider` in
  `MainWindow::setupClaudeMcpProviders`. Not ETag-eligible and not
  field-projected: a mutating verb is neither.
- **`docs/subsystems.md`** gains the migration lane, alongside the
  `roadmapsource` entry ANTS-3825 already owes it.
- **`CLAUDE.md`** — no change. Its module map is a pointer since ANTS-1292,
  and the verb catalogue is `tool_info {catalog:true}`.
- **ANTS-3807's bullet** currently attributes the missing cutover route to
  ANTS-3793 and ANTS-3794. It is amended to name this id, which is what
  actually blocked it.
- **`CHANGELOG.md`** — one `Added` entry.
- **A source-scrape caveat, not a doc.** Adding a verb has twice pushed a
  literal past a fixed-byte scrape window in `test_claude`
  (`kindForName`'s window, the `tool_prefix_tags` and `build_status` blocks).
  Re-run `test_claude` after the schema lands and widen any window that reds.

## Cold-eyes loop log

| Loop | Date | Lanes | Findings (C/H/M/L/I) | Resolution |
|---|---|---|---|---|
