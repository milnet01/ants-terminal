# ANTS-3855 — `roadmap_migrate`: the production entry point that loads a project into the store

**Status:** spec draft (2026-08-06).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3855 (in-session-2026-08-06, measured while starting ANTS-3853's first item).
**Blocker for:** ANTS-3807 (per-project migration briefs), ANTS-3772, ANTS-3815.
**Composes with:** ANTS-3757 (read half), ANTS-3765 (load half), ANTS-3793 (consumer cutover).

**Layman:** The roadmap database is built and tested but nothing can put a
project into it. This adds the command that does — with a preview mode that
reports exactly what it would change without saving any of it.

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
Handler `RemoteControl::cmdRoadmapMigrate` in a new translation unit (TU)
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
| `dry_run` | bool | `false` | Plan every write, report the counts, roll back. Declared via `makeDryRunProp()`. |
| `project_name` | string | leaf dir of the canonical root, verbatim | `project.name`. Must be non-empty after trimming — `project.name` is `TEXT NOT NULL`, and an all-whitespace name would satisfy the column and identify nothing. |
| `export_slug` | string | slugified leaf dir | `project.export_slug`. |

`project_name` and `export_slug` are arguments because **nothing derives
them**: `RoadmapMigrate::planFrom()` takes both from its caller by design
(`src/roadmapmigrate.h` — "`projectName` and `exportSlug` are supplied by the
caller, not derived … whose charset the store constrains and which nothing in
a markdown file carries"). The defaults follow `roadmap_log`'s existing
precedent of deriving from `caller_cwd`'s leaf directory rather than asking.

**Slugification** lowercases, replaces every run of non-`[a-z0-9]` with a
single `-`, and strips leading/trailing `-`. `Ants_Terminal` → `ants-terminal`.

**The validation rule is the DDL's own `CHECK`, quoted rather than
paraphrased** — a second spelling of a charset is a second charset:

```sql
export_slug TEXT NOT NULL UNIQUE
  CHECK (export_slug GLOB '[a-z0-9]*'
     AND export_slug NOT GLOB '*[^a-z0-9-]*')
```

So: first character alphanumeric, every character in `[a-z0-9-]`, non-empty.
A slug that survives slugification but fails this (a leaf directory of only
punctuation slugifies to the empty string) is `bad_args`, not a store error.

#### 2.1.1 The seam the handler composes, and why it is a free function

`cmdRoadmapMigrate` resolves `RoadmapStore::defaultPath()` and nothing else
of substance. The work is a free function in the same TU:

```cpp
// src/remotecontrol_roadmap_migrate.h — the testable seam.
namespace RoadmapMigrateVerb {

struct Request {
    QString projectRoot;      // already canonical
    QString projectName, exportSlug;
    QString changedAt;        // the caller's single stamp
    bool    dryRun = false;
};

// `storePath` is a PARAMETER, not RoadmapStore::defaultPath(). Returns the
// success envelope, or a refusal carrying `code`.
QJsonObject run(const QString &storePath, const Request &req);

}  // namespace RoadmapMigrateVerb
```

**`storePath` is a parameter for one reason: without it this verb has no test
at all.** `RoadmapStore::defaultPath()` resolves under `XDG_DATA_HOME` with no
override, so a test driving a handler that called it directly would migrate
into the user's real store — which is exactly how ANTS-3856's leaked fixture
row got there. A test that cannot be written without damaging the machine it
runs on is not a test.

This is the shape ANTS-3793 § 2.2 already used, for the same reason and in
almost the same words: `RoadmapSource::storeFor()` "is a free function and not
a wrapper member for one reason: INV-1's two unmigrated-project cases assert
exactly this decision, and a member of `RemoteControl` … is not reachable from
§ 6's bundle. The wrapper still OWNS the decision — it is the only caller —
but the decision is testable on its own." Here the handler still owns which
store is migrated; `run()` owns everything that happens to it.

### 2.2 The connection is the verb's own, and it is `Access::Bulk`

`RoadmapMigrateVerb::run()` constructs and opens its **own** `RoadmapStore` at
the `storePath` it was given, on `Access::Bulk`, for the duration of one call.
It must **not** reuse `RemoteControl::roadmapStoreOrNull()`, which is the
process-owned `Access::Interactive` connection ANTS-3793 § 2.2 opened for the
consumers: `RoadmapMigrateLoad::load()` refuses a non-`Bulk` store outright
(`src/roadmapmigrateload.cpp` — "a migration load needs an `Access::Bulk`
store", ANTS-3765 INV-12). Being a free function that takes a path,
`run()` *cannot* reach the process-owned connection — the rule is enforced by
the signature rather than by discipline, which is what INV-2 asserts.

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

```
HANDLER — RemoteControl::cmdRoadmapMigrate
 0a. caller_cwd absent or empty                       -> caller_cwd_required
 0b. root = ants::resolveCallerCwdRoot(caller_cwd)
     root empty (present but does not resolve)        -> no_project
 0c. RoadmapMigrateVerb::run(RoadmapStore::defaultPath(), {root, ...})

SEAM — RoadmapMigrateVerb::run(storePath, req)
 1. name = req.projectName, trimmed                   // empty -> bad_args
 2. slug = req.exportSlug                             // fails the CHECK -> bad_args
                                                      //   NOTHING opened yet
 3. disc = RoadmapMigrate::findRoadmaps(root, &code)  // nullopt -> refuse on `code`
 4. plan = RoadmapMigrate::planFrom(*disc, name, slug)  // pure; cannot fail
 5. store(storePath, kDefaultHistoryCapBytes, Access::Bulk)
    store.open()                                      // fails -> store_failed
 6. owner = store.readProjectByRoot(root)             // the re-run case
    other = store.readProjectBySlug(slug)
    other set AND other->root != root                 -> slug_collision
 7. opts = { changedAt: <one stamp>, projectRoot: root, dryRun: req.dryRun }
 8. out  = RoadmapMigrateLoad::load(store, plan, opts)
 9. envelope from `out`                               // out.ok false -> migrate_failed
```

The block is prose, not compilable C++ — an unlabelled fence, because a `cpp`
tag on numbered steps invites a reader to paste it.

**Steps 0a and 0b are two refusals, not one.** An absent or empty `caller_cwd`
is `caller_cwd_required` — the caller named no project. A `caller_cwd` that is
present but does not canonicalise is `no_project` — the caller named one that
is not there. `ants::resolveCallerCwdRoot` returns empty for both, so the
discriminator is the **argument**, checked before the call, not its result.

**Steps 1–2 precede step 5 deliberately.** An invalid slug that reached
`registerProject()` would fail the DDL's `export_slug` CHECK *inside* the
transaction and roll back a whole migration, reporting a store error for what
is an argument error. Validating first refuses in microseconds (INV-6).

**Step 6 exists because `export_slug` is `UNIQUE` and the default is derived.**
Two project roots whose leaf directories slugify alike — `…/foo` and
`…/Foo!` — both default to `foo`, and the second would otherwise fail inside
`registerProject()` and surface as the catch-all `migrate_failed`. That is the
same loss of the operator's next step that § 2.5 refuses to accept for
`case_ambiguous`, so it gets the same treatment: a cheap pre-check and the
canonical `slug_collision`. `root` is `UNIQUE` too, but a matching root is the
**re-run** case (INV-7) and not a collision — which is why step 6 compares
`other->root` rather than merely finding a row.

**A re-run may not silently change a registered project's identity.** When
step 6's `owner` exists and its `exportSlug` differs from `slug`, the call
refuses `slug_collision` as well: an omitted `export_slug` on the second run
recomputes the default, and a project that had been migrated under an explicit
slug would otherwise be quietly re-slugged. Changing a project's slug is a
deliberate act with an export path hanging off it, not a side effect of
leaving an argument out.

#### 2.3.1 `dry_run` opens the store, and may create it

This is a stated deviation from `mcp-tools.md`'s `dry_run` contract — "it
computes the would-be result envelope … and returns it *before* any disk
write" — recorded here rather than left for a reader to discover.

`store.open()` at step 5 runs `mkpath`, `createSchema()` and
`setOwnerOnlyPerms()` on the store and both WAL sidecars. On a machine whose
store does not exist yet, a `dry_run:true` call therefore **creates an empty
schema-initialised store**.

**It is required for the preview to be correct, which is why the deviation is
accepted rather than engineered around.** `load()`'s counts are a *diff*
against what the store already holds — `itemsUpdated` and `itemsUnchanged`
(ANTS-3765 § 2.6) are meaningless without the existing rows to match against.
A preview that opened a throwaway store instead would report every item as an
insert on a project that is already migrated: a confident, wrong answer, which
is worse than a documented side effect.

The deviation is bounded, and the bound is what makes it acceptable:

- What a dry run may create is an **empty schema** — zero `project`, `section`,
  `item`, `element` and `history` rows. It writes no roadmap data.
- It never modifies a store that already exists beyond what a rolled-back
  transaction leaves (the WAL and SQLite's change counter).
- The next real run would create exactly the same schema.

INV-3 asserts the row-level half of this, and INV-6 is stated against *rows*
rather than against the file's existence for the same reason.

**The stamp is taken once, at step 7** —
`QDateTime::currentDateTimeUtc().toString(Qt::ISODate)`, the same expression
`src/auditautofix.cpp` already ships. It yields the exact shape
`history.changed_at` CHECKs and `isIsoZStamp()` validates
(`^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`, `src/roadmapmigrateload.cpp`) —
run against Qt 6 on 2026-08-06 rather than assumed, because `Qt::ISODate`
would strip the `Z` on a local-time `QDateTime` and `Qt::ISODateWithMs` would
add milliseconds the regex rejects:

```console
stamp   = 2026-08-06T20:22:28Z
matches = YES
```

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
  "notes_count": 0, "notes_truncated": false
}
```

The example is the **shape**, not a measurement — every numeric field is
zeroed so no figure here can be read as this project's, `project_id` included.
§ 4 carries the measured ones, with the run that produced them.

`sources[].path` is **project-relative** (`ROADMAP.md`,
`docs/roadmap/0.6.md`), not the absolute path `findRoadmaps()` resolved. The
caller supplied the root; echoing it back on every entry is noise, and a
relative path is what the operator would type.

`project_id` is the store's `project_id` after a committed run. **Under
`dry_run` it is `0`** — `registerProject()`'s rowid is allocated inside a
transaction that is about to roll back, and a provisional id that a later real
run need not reuse is worse than no id, because it looks durable.

`notes[]` is capped at **200 entries**, with `notes_truncated: true` when the
cap bites; `notes_count` is always the **true** total, so a truncated report
still says how much it dropped. The cap exists because the array is otherwise
unbounded — one note per offending line, and ANTS-3772's 3D_Engine produced 17
id collisions on its own — and this project ships no unbounded growth without a
named cap. 200 is chosen to sit well above the worst observed real project
while keeping the envelope small enough not to need the offload path; a project
that exceeds it has a systemic problem the first 200 notes already describe.

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

| Step | Condition | `code` |
|---|---|---|
| 0a | `caller_cwd` absent or empty | `caller_cwd_required` |
| 0b | `caller_cwd` present but does not resolve | `no_project` |
| 1 | `project_name` empty after trimming | `bad_args` |
| 2 | `export_slug` fails the DDL `CHECK` in § 2.1 | `bad_args` |
| 3 | `findRoadmaps()` → `not_found` | `no_roadmap` |
| 3 | `findRoadmaps()` → `case_ambiguous` | `case_ambiguous` |
| 3 | `findRoadmaps()` → `not_utf8` | `not_utf8` |
| 3 | `findRoadmaps()` → `archive_format_mismatch` | `format_mismatch` |
| 5 | `store.open()` fails | `store_failed` |
| 6 | the slug belongs to a different root, or a re-run changes this root's slug | `slug_collision` |
| 8 | `load()` returns `ok == false` | `migrate_failed`, `error` = `Outcome::error`, `notes` carried |

Every refusal carries `ok:false`, a `code`, and a human-readable `error`.

**The mapping rule, stated once so the table does not read as ad-hoc:** a
`findRoadmaps()` code maps onto a canonical code when one already means the
same thing, and is added to the taxonomy when none does. `not_found` →
`no_roadmap` and `archive_format_mismatch` → `format_mismatch` are the first
case: the canonical codes say exactly that, and minting a synonym would split
one meaning across two codes. `case_ambiguous` and `not_utf8` are the second —
nothing canonical means "two roadmap files differ only in case" or "the bytes
are not UTF-8", and folding them into `no_roadmap` or `parse_failed` would
lose the distinction the operator's next step turns on. `migrate_failed` is
likewise new: no canonical code means "the store is fine, the source is fine,
and the load refused".

**Rows do not change on any refusal.** Steps 0a–4 precede any store open at
all. Steps 5–8 may create an empty schema (§ 2.3.1) but commit no `project`,
`section`, `item`, `element` or `history` row: step 8 is one transaction per
project, so `Outcome::ok == false` means "NOTHING for this project was
committed" (`src/roadmapmigrateload.h`). INV-4 states it at that row level,
not as a byte comparison — `store.open()` legitimately creates the file, and a
rolled-back transaction legitimately moves the WAL and the change counter, so
a byte-identity claim would red against a correct implementation.

### 2.6 Trust boundary

The verb crosses three (`specs.md` § 5.4), and re-states the existing defences
rather than relying on them silently. It adds no new defence — the point of
naming them is that a change here must not weaken one.

- **IPC / caller input.** `caller_cwd`, `project_name` and `export_slug` arrive
  over the MCP socket from an agent. `caller_cwd` is a path-typed argument and
  routes through `ants::resolveCallerCwdRoot` (ANTS-1401), which owns
  canonicalisation and the root walk; the verb never joins or normalises a path
  itself. `export_slug` is constrained to `[a-z0-9-]` before it reaches SQL
  (§ 2.1) and every store write is a bound `QSqlQuery` parameter, so the slug
  is never string-built into a statement.
- **Filesystem read.** `findRoadmaps()` reads only under the resolved root and
  the `docs/roadmap/` beside it (ANTS-3757 § 2.2). The verb passes the root and
  reads nothing itself.
- **Store write.** `RoadmapStore::open()` applies `setOwnerOnlyPerms()` (0600)
  to the store and both WAL sidecars — ANTS-3756 INV-17, on the ground that the
  sidecars carry the same content including `visibility:internal` items. A
  store this verb **creates** is subject to that, which is why creation goes
  through `open()` and never through a bespoke path. INV-9 locks it.

## 3. Invariants

Every `*Test:*` clause below drives `RoadmapMigrateVerb::run(storePath, req)`
against a `QTemporaryDir` store, never `RoadmapStore::defaultPath()` — § 2.1.1
is what makes that possible and § 6 states the one exception (INV-1, a
source-grep).

- **INV-1** — A non-test translation unit calls all three migration entry
  points, and only the verb's TU does. *Test:* source-grep in the feature test,
  running § 1's two commands unchanged: each names
  `src/remotecontrol_roadmap_migrate.cpp` and no other `src/` file. The
  `grep -v ': *//'` filter drops only comment-*leading* lines, so a future
  block-comment mention of the call would red it spuriously — the grep is
  written to match `Name(` and the filter is a convenience, not the contract.
- **INV-2** — `run()` migrates on an `Access::Bulk` connection it opened
  itself, and cannot reach the process-owned `Access::Interactive` one.
  *Test:* two legs, because `ok:true` alone would also pass for a *Bulk*
  process-owned connection. (a) The feature test calls `run()` against a fresh
  temp store and asserts `ok:true` — an `Interactive` connection is refused by
  `load()`'s INV-12 check, so the two outcomes differ. (b) A source-grep
  asserts `src/remotecontrol_roadmap_migrate.cpp` never names
  `roadmapStoreOrNull`, and that `run()`'s signature takes a `storePath`
  rather than a `RoadmapStore &`.
- **INV-3** — `dry_run:true` commits no row and reports the counts the real run
  produces. *Test:* feature test — dry run against a temp store, then assert
  `SELECT COUNT(*)` is 0 on each of `project`, `section`, `item`, `element` and
  `history` (via `store.db()`); then the real run, and assert every count field
  in the envelope equals the dry run's (ANTS-3765 INV-13's comparison, driven
  through the verb).
- **INV-4** — No refusal in § 2.5 leaves a committed row: after any of them,
  the store holds no `project` row for this root and no `section` / `item` /
  `element` / `history` row referencing one. *Test:* feature test — drive each
  refusal in turn against a temp store, asserting the five counts are 0 after
  each. Stated at row level, not as a byte hash: `store.open()` legitimately
  creates the file and a rollback legitimately moves the WAL (§ 2.5).
- **INV-5** — One stamp per call, in the shape `history.changed_at` CHECKs.
  *Test:* the envelope's `changed_at` matches
  `^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`, and
  `SELECT COUNT(DISTINCT changed_at) FROM history` is ≤ 1 after a single run
  that wrote history rows. `RoadmapStore` exposes no history *reader*, so the
  test queries through the public `store.db()`.
- **INV-6** — An invalid `export_slug` or empty `project_name` refuses
  `bad_args` with no store connection opened. *Test:* feature test —
  `export_slug: "Ants_Terminal"` against a temp-dir path that does not yet
  exist; assert `bad_args` **and that no file appeared at that path**. Stated
  against a path the test controls, so it is satisfiable on a machine whose
  real store already exists.
- **INV-7** — A second run over an unchanged project is idempotent:
  `items_inserted == items_updated == items_orphaned == ids_allocated == 0`,
  and the project still has exactly one row. *Test:* feature test runs `run()`
  twice over one fixture root. (`elements_written` is deliberately excluded —
  `roadmapmigrateload.h` states it "is non-zero even on an unchanged re-run".)
- **INV-8** — After a successful run the consumer path serves that project from
  the store in the same process, with no restart. *Test:* feature test —
  `RoadmapSource::migratedProject(store, root, markdown)` is `nullopt` before
  the run and returns the project id after, against the same temp store. Driven
  at the seam rather than through `RemoteControl::roadmapStoreServes()`, which
  resolves `defaultPath()` and so cannot be aimed at a fixture; § 2.2's
  "absence is NOT remembered" is the property under test, and
  `migratedProject()` is what that member calls.
- **INV-9** — A store this verb creates is mode 0600, and so are its `-wal` and
  `-shm` sidecars. *Test:* feature test — `run()` against a temp path with no
  store, then `QFile::permissions()` on all three is owner-read/write only.
  Locks § 2.6's third boundary: creation goes through `RoadmapStore::open()`,
  which applies ANTS-3756 INV-17, and never through a bespoke path.

## 4. RAM / build cost

**Memory.** One `Access::Bulk` connection's **16 MiB** page cache
(`kBulkCacheKiB`, ANTS-3756 § 2.5) plus one plan, for the duration of one
call, then released (§ 2.2 owns why the connection is per-call). ANTS-3757 § 4
budgets the plan itself; nothing here holds state between calls. The one
bounded accumulation in the response is `notes[]`, capped at 200 (§ 2.4).

**Time — the ceiling is ANTS-3765 § 4's 1 s per project, inherited, not the
bridge timeout.** That spec sets it because "the write lock is held for a whole
project (§ 2.5), so a project slower than that is starving a concurrent
export", and this verb holds the same lock. The MCP bridge's 60 s call timeout
(ANTS-3444) is the outer bound only — it is what makes the verb synchronous,
with no job/poll surface, and it is not a budget anything can red against.

Measured 2026-08-01 by ANTS-3765 § 4 (quiet machine, two runs agreeing within
6 ms) over the ten real project roots under `/mnt/Games/Scripts/Linux/`: the
worst project is **Ants_Terminal at 129 ms** (1,794 items / 1,888 elements,
2.9 MB of source), re-run 134 ms.

**Two costs sit outside that figure and are stated rather than assumed.** That
run drove `load()` through **one** `Bulk` connection shared across ten
projects, so the per-call `mkpath` + `addDatabase` + `applyPragmas` +
`createSchema` + three `setOwnerOnlyPerms` this verb pays is additive and
unmeasured; so is `findRoadmaps()`' own read of the source files. Both are
bounded by a handful of syscalls and one 3 MB read, which is why the 1 s
ceiling is kept rather than loosened — but the honest statement is that
129 ms is the load's cost, not the verb's. **The verb's own end-to-end figure
is measured at implementation and folded back here** (`/write-spec` Step 8);
until then the ceiling is the contract and the 129 ms is its provenance.

**Build.** One new `.cpp` in an existing library. No new target, no new
dependency, no new link edge (§ 2.1). The feature test joins `test_core`'s
`SOURCES` per `tests/features/README.md`.

## 5. Out of scope

- **A dialog action.** A permanent exclusion, not deferred work: the MCP verb
  is the surface ANTS-3807's per-project briefs invoke, and this spec is not
  the contract for a UI nobody has asked for. Should a button ever be wanted it
  calls `RoadmapMigrateVerb::run()` and needs its own item; it does not need a
  placeholder id filed here against work nobody intends to do.
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

Feature test: `tests/features/roadmap_migrate_verb/`. Covers INV-1..INV-9.
Label `features;fast`. Source added to `test_core`'s `SOURCES` list — not
`add_executable` (`tests/features/README.md`).

**One testing route, and § 2.1.1 is what provides it: every behavioural
invariant drives `RoadmapMigrateVerb::run(storePath, req)` directly**, with
`storePath` inside a `QTemporaryDir`. The registered handler
(`cmdRoadmapMigrate`) is *not* driven by any test, because its whole remaining
job is to resolve `RoadmapStore::defaultPath()` — the user's real store, which
no test may write to. That one line is covered by INV-2's source-grep leg
instead.

That split is the reason the seam takes a path at all. A test that drove the
handler would migrate into `~/.local/share/ants-terminal/roadmap.sqlite` and
reproduce ANTS-3856's leaked fixture row exactly.

| Invariant | Route |
|---|---|
| INV-1 | source-grep only |
| INV-2 | (a) `run()` against a temp store · (b) source-grep |
| INV-3..INV-9 | `run()` against a temp store |

Each invariant is verified to FAIL against pre-change source before the code
is restored, per the project test convention. INV-1 and INV-2(b) fail
trivially (no such TU); INV-2(a) and INV-3..INV-9 fail to compile against a
tree with no `RoadmapMigrateVerb::run()`, which is the must-fail-first proof
for a new surface.

Fixture: a `QTemporaryDir` project root carrying a small hand-written
`ants-v1` `ROADMAP.md`, plus a store path inside that same temp dir — created
by the verb, not by the fixture, so INV-6 and INV-9 can assert on what
creation does.

## 7. Cross-doc impact

- **`docs/standards/mcp-error-codes.md`** gains `case_ambiguous`, `not_utf8`
  and `migrate_failed` (§ 2.5).
- **`docs/standards/mcp-tools.md`** gains one line: `roadmap_migrate` joins the
  `dry_run` support list, and § 2.3.1's deviation is recorded beside it — the
  standard says a preview returns "before any disk write", and this verb may
  create an empty schema. A deviation a standard does not know about is a
  standard that is quietly wrong.
- **The authoring checklist is walked**, and these are the registration points
  it names for this verb: `tools/list` schema entry;
  `callerCwdContractFor` → `Required`; `kindForName` bucketing; `tokenCostFor`;
  `registerToolProvider` in `MainWindow::setupClaudeMcpProviders`;
  `makeDryRunProp()` for the `dry_run` prop (this verb is new, so it does not
  inherit the tailored copies `roadmap_log` / `changelog_log` / `spec_log`
  keep); and `ants::resolveCallerCwdRoot` for the one path-typed argument —
  `caller_cwd`, which ANTS-1401 owns, so `PathValidation::validatePath` is not
  called directly here. Not ETag-eligible and not field-projected: a mutating
  verb is neither.
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
| 1 | 2026-08-06 | 3 cold `general-purpose`, one shared byte-identical packet | C 3 · H 4 · M 6 · L 10 · I 1 — **23 verified, 3 dismissed** | All 23 fixed. Dimension tally: dim 15×5, dim 5×5, dim 4×3, dim 7×3, dim 10×3, dim 1×2, dim 6×2, dim 9×2, dim 12×2, dim 8×1, dim 13×1. **All three lanes independently led on the same defect**, which is what makes the tail credible: § 6 said INV-2..INV-8 were "driven through the registered handler" and then that the handler resolves `defaultPath()` internally, so six invariants had no runnable test surface and an implementer following ¶1 would have migrated into the user's real store — reproducing ANTS-3856. Fixed by § 2.1.1's `RoadmapMigrateVerb::run(storePath, req)` seam, on the precedent ANTS-3793 § 2.2 set for `storeFor()`. Two more the draft got flatly wrong: INV-4 claimed the store is "byte-unchanged" after *every* refusal, false for the two that follow `store.open()` (which creates the file and both WAL sidecars) — restated at row level; and `dry_run` was never reconciled with `mcp-tools.md`'s "returns before any disk write", which it violates by opening the store — now § 2.3.1, stated as a bounded deviation with the argument for why a throwaway store would make the preview *wrong* rather than merely different. Also added: a `slug_collision` pre-check (`export_slug` is `UNIQUE` and the default is derived, so two roots slugifying alike collided into the catch-all), § 2.6's trust boundary + INV-9 (`specs.md` § 5.4 requires it and the draft had none), a 200-entry `notes[]` cap, and the 1 s/project ceiling ANTS-3765 § 4 sets — the draft had quoted only the 60 s bridge timeout, 60× looser. **Dismissed on verification:** no-TOC (no sibling spec carries one and `specs.md` § 3's required order omits it); a claimed-stale "4 MiB" comment in `roadmapstore.cpp` (lane misread — the 4 MiB is ANTS-3761 INV-12's export RSS budget, not the cache size); `features;fast` not being a real label (it is — `CMakeLists.txt:1001`). **Found during packet construction, before a lane was spent:** nothing. **Found by 4b's sweep, not by a lane:** the `**Layman:**` line still promised a preview that writes nothing, contradicting the § 2.3.1 just added, and two sub-subsections used `###` where siblings use `####`. **Executed rather than read** (4a step 2): the `export_slug` `CHECK` against real SQLite (`Ants_Terminal` fails both clauses, `ants-terminal` passes both, empty fails — so INV-6's fixture is valid), and `QDateTime::currentDateTimeUtc().toString(Qt::ISODate)` compiled against Qt 6, returning `2026-08-06T20:22:28Z`, which `isIsoZStamp()`'s regex accepts. Doc grew 318 → 575 lines; watch it, and split at § 2's seams if loop 2 is still finding structural defects. |
