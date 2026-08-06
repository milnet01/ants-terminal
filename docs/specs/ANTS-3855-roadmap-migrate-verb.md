# ANTS-3855 — Add `roadmap_migrate`, the verb that loads a project into the store

**Status:** accepted (2026-08-06) — cold-eyes loops 1–3, converged by cap, no deferred findings.
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

**Slugification applies to the DERIVED DEFAULT only. A caller-supplied
`export_slug` is validated verbatim and never rewritten** — silently reshaping
an argument the caller chose would make the value in the store differ from the
value they passed, and they have an export path keyed on it. So a supplied
`Ants_Terminal` is `bad_args` (INV-6), not a quietly-accepted `ants-terminal`.

**The validation rule is the DDL's own `CHECK`, quoted rather than
paraphrased** — a second spelling of a charset is a second charset:

```sql
export_slug TEXT NOT NULL UNIQUE
  CHECK (export_slug GLOB '[a-z0-9]*'
     AND export_slug NOT GLOB '*[^a-z0-9-]*')
```

So: first character lowercase-alphanumeric, every character in `[a-z0-9-]`,
non-empty. A slug that survives slugification but fails this — a leaf directory
of only punctuation slugifies to the empty string — is `bad_args`, not a store
error.

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
store", ANTS-3765 INV-12). `run()` *cannot* reach the process-owned
connection: it is a free function taking a path, so its signature enforces the
rule. The handler in the same TU does hold `this`, which is why INV-2(b) greps
the TU as well rather than resting on the signature alone.

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
in the same session. **ANTS-3793 already guarantees that and this spec relies
on it; INV-8 asserts only the store-side half** (the project resolves through
the dispatch afterwards), because no fixture here can observe another object's
cache.

**`run()` releases its connection before it returns.** The `RoadmapStore` is a
stack local, and `~RoadmapStore()` runs `PRAGMA optimize`, `m_db.close()` and
`QSqlDatabase::removeDatabase(m_connName)` — so the per-instance connection
names do not accumulate across calls, and the WAL sidecars are checkpointed
away. INV-9 depends on that ordering.

### 2.3 The sequence, and where each refusal falls

```
HANDLER — RemoteControl::cmdRoadmapMigrate
 0a. rr = ants::resolveCallerCwdRoot(m_main, caller_cwd)
     rr.source == Unresolvable                        -> no_project
 0b. stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate)  // ONE clock read
     RoadmapMigrateVerb::run(RoadmapStore::defaultPath(),
                             {rr.cwd, name, slug, stamp, dryRun})

SEAM — RoadmapMigrateVerb::run(storePath, req)
 1. name = req.projectName, trimmed                   // empty -> bad_args
 2. slug = req.exportSlug                             // fails the CHECK -> bad_args
                                                      //   NOTHING opened yet
 3. disc = RoadmapMigrate::findRoadmaps(root, &code)  // nullopt -> refuse on `code`
 4. plan = RoadmapMigrate::planFrom(*disc, name, slug)  // pure; cannot fail
 5. store(storePath, RoadmapStore::kDefaultHistoryCapBytes, Access::Bulk)
    store.open()                                      // fails -> store_failed
 6. other = store.readProjectBySlug(slug, &sqlErr)    // sqlErr set -> store_failed
    other set AND other->root != root                 -> slug_collision
    owner = store.readProjectByRoot(root, &sqlErr)    // sqlErr set -> store_failed
    owner set AND owner->exportSlug != slug           -> slug_collision
    owner set AND owner->name != name                 -> bad_args
 7. opts = { changedAt: <one stamp>, projectRoot: root, dryRun: req.dryRun }
 8. out  = RoadmapMigrateLoad::load(store, plan, opts)
                                                      // out.ok false -> migrate_failed
 9. envelope from `out`
```

The block is prose, not compilable C++ — an unlabelled fence, because a `cpp`
tag on numbered steps invites a reader to paste it.

**`caller_cwd` absent is not this verb's refusal to make.** `CallerCwdContract`
`Required` (§ 2.1) means the dispatcher refuses an empty `caller_cwd` with
`caller_cwd_required` before the handler is entered (`mcp-tools.md`,
ANTS-1404), so step 0a sees only a non-empty argument and has exactly one case
of its own: `ResolvedRoot::Source::Unresolvable`, which
`src/resolvedroot.h` defines as "`caller_cwd` present but
`QFileInfo::canonicalFilePath()` returned empty (path doesn't exist)" →
`no_project`. `NoMatch` (resolves, but no open tab sits there) is **not** a
refusal: migrating a project you do not have a terminal open in is legitimate.

**`rr.cwd` is canonical, and that is a precondition three later steps rest
on.** `src/resolvedroot.h` documents the field as "Canonical FS path" and
derives `Unresolvable` from `QFileInfo::canonicalFilePath()` returning empty —
the *same* call `RoadmapSource::migratedProject()` makes and the same one
`registerProject()` applies internally (ANTS-3756 INV-8). So the string step 6
looks up by, the string `load()` stores, and the string the read seam later
resolves are one form. Were they to diverge — a trailing separator, a symlink
policy — step 6's re-run detection would miss, every re-run would attempt a new
project row, and INV-7's idempotency would fail. `run()` therefore takes an
already-canonical root and does not re-canonicalise: one canonicaliser, named.

**Steps 1–2 precede step 5 deliberately.** An invalid slug that reached
`registerProject()` would fail the DDL's `export_slug` CHECK *inside* the
transaction and roll back a whole migration, reporting a store error for what
is an argument error. Validating first refuses in microseconds (INV-6).

**Both step-6 lookups check their error out-param, not just their `optional`.**
`readProjectBySlug()` / `readProjectByRoot()` return `nullopt` for *both* "no
such row" and "the query failed", and conflating them would read an SQL failure
as "not registered" — the collision guard would then be skipped and the failure
would resurface at step 8 as a UNIQUE violation wearing `migrate_failed`. This
is the same split `RoadmapSource::migratedProject()` already makes
(`if (!sqlError.isEmpty()) { *why = StoreFailed; … }`), and it is made the same
way here.

**Step 6 exists because `export_slug` is `UNIQUE` and the default is derived.**
Two project roots whose leaf directories slugify alike — `…/foo` and
`…/Foo!` — both default to `foo`, and the second would otherwise fail inside
`registerProject()` and surface as the catch-all `migrate_failed`. That is the
same loss of the operator's next step that § 2.5 refuses to accept for
`case_ambiguous`, so it gets the same treatment: a cheap pre-check and the
canonical `slug_collision`. `root` is `UNIQUE` too, but a matching root is the
**re-run** case (INV-7) and not a collision — which is why step 6 compares
`other->root` rather than merely finding a row.

**A re-run may not silently change a registered project's identity, and
identity is both fields.** `project_name` and `export_slug` are each defaulted
from the leaf directory, so an argument omitted on the second run recomputes
its default — and a project migrated under an explicit value would otherwise be
quietly re-slugged or renamed. Step 6 refuses both, with different codes
because the consequences differ: a changed `export_slug` is `slug_collision`
(an export path hangs off it), a changed `project_name` is `bad_args` (a
display-name change nobody asked for). Either is a deliberate act, not a side
effect of leaving an argument out. Re-running with the *same* values is the
idempotent path INV-7 covers.

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

INV-3 asserts the row-level half of this, and INV-4 is stated against *rows*
rather than against the file's bytes for the same reason. INV-6 is the
deliberate exception: it fires at step 2, before anything opens the store at
all, so there file existence *is* the observable and asserting it is what
proves nothing was opened.

#### 2.3.2 The stamp

**The clock is read exactly once, by the HANDLER at step 0b** —
`QDateTime::currentDateTimeUtc().toString(Qt::ISODate)`, the same expression
`src/auditautofix.cpp` already ships. `run()` never reads a clock: it receives
the value as `Request::changedAt` and forwards it to `Options::changedAt` at
step 7. That is what makes `run()` reproducible — a test pins `changed_at` by
passing it — and it is ANTS-3765 § 2.1's rule ("the clock is a PARAMETER, not
a call") held one layer further out. A `run()` that stamped itself would put
the non-determinism back exactly where that spec removed it.

The expression yields the exact shape
`history.changed_at` CHECKs and `isIsoZStamp()` validates
(`^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`, `src/roadmapmigrateload.cpp`) —
run against Qt 6 on 2026-08-06 rather than assumed, because `Qt::ISODate`
would strip the `Z` on a local-time `QDateTime` and `Qt::ISODateWithMs` would
add milliseconds the regex rejects:

```console
stamp   = 2026-08-06T20:22:28Z
matches = YES
```

A handler that stamped per row rather than per call would defeat the same
property from the outside, which is why step 0b builds one value and nothing
downstream re-derives it.

### 2.4 Response

```json
{
  "ok": true,
  "dry_run": false,
  "project_id": 0,
  "project_name": "Ants_Terminal",
  "export_slug": "ants-terminal",
  "store_path": "/home/…/.local/share/ants-terminal/roadmap.sqlite",
  "changed_at": "2026-08-06T21:14:07Z",
  "sources": [{"path": "ROADMAP.md", "format": "ants-v1"}],
  "items_inserted": 0, "items_updated": 0, "items_unchanged": 0,
  "items_orphaned": 0, "ids_allocated": 0,
  "sections_written": 0, "elements_written": 0, "history_rows": 0,
  "notes": [], "notes_count": 0, "notes_truncated": false
}
```

The example is the **shape**, not a measurement — every numeric field is
zeroed and `notes[]` is empty, so no figure here can be read as this project's,
`project_id` included. § 4 carries the measured ones.

`sources[].path` is **relative to `req.projectRoot`** — the canonical root of
§ 2.3 — so `ROADMAP.md` and `docs/roadmap/0.6.md`, not the absolute paths
`findRoadmaps()` resolved. The caller supplied that root; echoing it back on
every entry is noise.

`project_id` is the store's `project_id` after a committed run. **Under
`dry_run` it is `0`** — `registerProject()`'s rowid is allocated inside a
transaction that is about to roll back, and a provisional id that a later real
run need not reuse is worse than no id, because it looks durable.

`notes[]` is bounded on **both axes**, because capping the element count alone
bounds no bytes — `Note::detail` is a `QString` with no length rule of its own:

| Bound | Value | On breach |
|---|---|---|
| entries | 200 | `notes_truncated: true`; `notes_count` stays the TRUE total |
| `detail` | 2 KiB each | that entry's `detail` is clipped, ellipsis appended |

So the array is ≤ ~400 KiB in the worst case and typically a few KiB. The cap
exists because one note is emitted per offending line — ANTS-3772's 3D_Engine
produced 17 id collisions on its own — and this project ships no unbounded
growth without a named cap. A project exceeding 200 has a systemic problem the
first 200 notes already describe.

A refusal envelope that carries `notes` (only `migrate_failed` does) carries
all three fields, under the same bounds.

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
| — | `caller_cwd` absent or empty — refused by the `Required` contract, before the handler | `caller_cwd_required` |
| 0a | `caller_cwd` present but does not canonicalise | `no_project` |
| 1 | `project_name` empty after trimming | `bad_args` |
| 2 | `export_slug` fails the DDL `CHECK` in § 2.1 | `bad_args` |
| 3 | `findRoadmaps()` → `not_found` | `no_roadmap` |
| 3 | `findRoadmaps()` → `case_ambiguous` | `case_ambiguous` |
| 3 | `findRoadmaps()` → `not_utf8` | `not_utf8` |
| 3 | `findRoadmaps()` → `archive_format_mismatch` | `format_mismatch` |
| 5 | `store.open()` fails | `store_failed` |
| 6 | either lookup fails with an SQL error (distinct from "no row") | `store_failed` |
| 6 | the slug belongs to a different root, or a re-run changes this root's slug | `slug_collision` |
| 6 | a re-run changes this root's `project_name` | `bad_args` |
| 8 | `load()` returns `ok == false` — including a lock timeout | `migrate_failed`, `error` = `Outcome::error`, `notes` carried |

Every refusal carries `ok:false`, a `code`, and a human-readable `error`.

**A concurrent writer surfaces as `migrate_failed`, deliberately and not by
omission.** `Access::Bulk` carries a 30 s busy deadline (ANTS-3756 § 2.5) and
the write lock is held for a whole project, so a second `roadmap_migrate` or a
concurrent export can outlast it. SQLite reports that to `load()`, which
returns `ok == false` with the driver's message in `Outcome::error`, and it
reaches the caller intact. It gets no code of its own because the operator's
next step is the same one `migrate_failed` already implies — read `error`, then
retry — and a `store_busy` code would suggest a different one. The 30 s
deadline sits inside the bridge's 60 s call timeout (§ 4), so the deadline
fires first and the caller gets this refusal rather than a dead socket.

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
  `<root>/docs/roadmap/` (ANTS-3757 § 2.2). The verb passes the root and
  reads nothing itself.
- **Store write.** `RoadmapStore::open()` applies `setOwnerOnlyPerms()` (0600)
  to the store and both WAL sidecars — ANTS-3756 INV-17, on the ground that the
  sidecars carry the same content including `visibility:internal` items. A
  store this verb **creates** is subject to that, which is why creation goes
  through `open()` and never through a bespoke path. INV-9 locks it.

## 3. Invariants

§ 6 owns the test routing; two facts it fixes are needed to read the clauses
below. Every behavioural clause drives `RoadmapMigrateVerb::run(storePath,
req)` with `storePath` inside a `QTemporaryDir` (INV-1 and INV-2(b) are
source-greps). And because `run()` owns its store for the duration of the call
and hands back only a `QJsonObject`, **a clause that inspects rows opens the
test's own `Access::Interactive` `RoadmapStore` at the same `storePath` after
`run()` returns**, querying through the public `RoadmapStore::db()`.

- **INV-1** — A non-test translation unit calls all three migration entry
  points, and only the verb's TU does. *Test:* source-grep in the feature test.
  The contract is the three call-shape patterns
  (`RoadmapMigrateLoad::load(`, `RoadmapMigrate::findRoadmaps(`,
  `RoadmapMigrate::planFrom(`): each must match
  `src/remotecontrol_roadmap_migrate.cpp` and no other file under `src/`.
  The test scans the tree in-process with `QFile` + `QRegularExpression`,
  skipping lines whose first **non-whitespace** characters are `//` — the two
  surviving mentions are indented member comments (`src/roadmapstore.h`,
  `src/roadmapmigrate.h`), so a literal "line begins with `//`" rule would red
  against correct code; § 1's `grep -v ': *//'` tolerates the indent for the
  same reason. It does **not** shell out to `rg`, which is not
  a declared build or test dependency and would make the leg pass vacuously
  where it is absent. The source root comes from a compile-time
  `ANTS_SRC_DIR` define, as the existing source-scrape tests do, because a
  bundle binary runs from the build tree and cannot assume its cwd. § 1's `rg`
  commands are the human-runnable form of the same check, not the test's
  mechanism.
  <br>The define already exists and needs no CMake change: `test_core` is
  given `ANTS_SRC_DIR="${CMAKE_SOURCE_DIR}/src"` for exactly this — "the source
  tree the refit scrape walks" (ANTS-3758 INV-11) — and this test joins that
  bundle.
- **INV-2** — `run()` migrates on an `Access::Bulk` connection it opened
  itself, and cannot reach the process-owned `Access::Interactive` one.
  *Test:* two legs, because `ok:true` alone would also pass for a *Bulk*
  process-owned connection. (a) The feature test calls `run()` against a fresh
  temp store and asserts `ok:true` — an `Interactive` connection is refused by
  `load()`'s INV-12 check, so the two outcomes differ. (b) A source-grep over
  `src/remotecontrol_roadmap_migrate.{h,cpp}` — INV-1's comment-skipping rule,
  because § 2.2's prose ("It must **not** reuse
  `RemoteControl::roadmapStoreOrNull()`") is exactly the kind of rationale the
  TU would carry as a comment — asserting that no *code* line names
  `roadmapStoreOrNull`, and that `run()`'s declared signature takes a
  `storePath` rather than a `RoadmapStore &`.
- **INV-3** — `dry_run:true` commits no row and reports the counts the real run
  produces. *Test:* feature test — dry run against a temp store, then assert
  `SELECT COUNT(*)` is 0 on each of `project`, `section`, `item`, `element` and
  `history` (via `store.db()`); then the real run, and assert every count field
  in the envelope equals the dry run's (ANTS-3765 INV-13's comparison, driven
  through the verb). Also asserts § 2.4's two dry-run envelope rules:
  `project_id` is `0` on the dry run and non-zero on the real one, and
  `sources[].path` is relative on both (`ROADMAP.md`, never an absolute path).
- **INV-4** — No refusal in § 2.5 **adds or changes** a row. *Test:* feature
  test — snapshot `SELECT COUNT(*)` on `project`, `section`, `item`, `element`
  and `history`, drive each refusal `run()` can reach (steps 1–8), re-snapshot,
  and assert all five are unchanged.
  <br>**Stated as "unchanged", not "zero", because two of § 2.5's refusals
  require rows to already exist**: a re-run that changes this root's slug or
  name, and a slug owned by a *different* root, all presuppose a `project` row —
  against those, a zero-count assertion is false for a correct implementation.
  Row level rather than a byte hash, for § 2.5's reason.
  <br>**Two rows are out of the fixture's reach and are covered by inspection,
  not by this test:** `caller_cwd_required` is the dispatcher's, before the
  handler; `no_project` is step 0a's, inside `cmdRoadmapMigrate`, which § 6
  drives no test against. Both refuse before `run()` is entered, so neither can
  touch a store — the property holds by construction rather than by fixture.
- **INV-5** — One stamp per call, in the shape `history.changed_at` CHECKs.
  *Test:* the envelope's `changed_at` matches
  `^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`; then, on a store this test alone
  created, `SELECT COUNT(DISTINCT changed_at) FROM history` is exactly 1 after
  one committed run that wrote history.
  <br>**The fixture must be a re-run over an EDITED source, not a first load.**
  `Loader::recordHistory()` is reached only from the field-update path in
  `src/roadmapmigrateload.cpp`, so a first-ever migration into an empty store
  inserts items and appends no `history` row at all — against which
  `history_rows > 0` is unreachable and a `COUNT(DISTINCT …)` guard would pass
  or red for a reason unrelated to the one-stamp rule. So: migrate, edit one
  item's headline in the fixture roadmap, migrate again with a *distinct*
  stamp, and assert on that second run.
- **INV-6** — An invalid `export_slug` or empty `project_name` refuses
  `bad_args` with no store connection opened. *Test:* feature test —
  two legs, each against a temp-dir store path that does not yet exist, each
  asserting `bad_args` **and that no file appeared at that path** — (a)
  `export_slug: "Ants_Terminal"`, (b) `project_name: "   "`. Stated against a
  path the test controls, so it is satisfiable on a machine whose real store
  already exists; and file existence is the right observable here precisely
  because these two refuse before anything opens a store (§ 2.3.1).
- **INV-7** — A second run over an unchanged project is idempotent:
  `items_inserted == items_updated == items_orphaned == ids_allocated ==
  sections_written == history_rows == 0`, and the project still has exactly
  one row. *Test:* feature test runs `run()`
  twice over one fixture root. (`elements_written` is the ONE count deliberately
  excluded — `roadmapmigrateload.h` states it "is non-zero even on an unchanged
  re-run" because § 2.6 rebuilds element rows wholesale.)
- **INV-8** — After a successful run, the project resolves through ANTS-3793's
  consumer dispatch against the same store. *Test:* feature test —
  the test opens its own `RoadmapStore` at `storePath` (creating it), asserts
  `RoadmapSource::migratedProject(store, root, markdown)` is `nullopt`, closes
  it, calls `run()`, then re-opens and asserts it returns the project id. (The three-argument call compiles:
  `error` and `why` both default to `nullptr` in `src/roadmapsource.h`.)
  <br>**Scoped to what this fixture can falsify.** The stronger property — that
  a *running session's* consumers pick the migration up with no restart — lives
  in `RemoteControl::roadmapStoreOrNull()`'s "absence is NOT remembered"
  caching (§ 2.2), which `migratedProject()` never touches and which this test
  would pass without. That property is ANTS-3793's and is already shipped; this
  spec relies on it and does not re-assert it.
- **INV-9** — A store this verb creates is mode 0600. *Test:* feature test —
  `run()` against a temp path with no store, then `QFile::permissions()` on the
  store file is owner-read/write only.
  <br>**The test sets `umask(022)` for the duration of the call and restores it
  after** (RAII, per the project's test-env convention). Without that the leg
  passes vacuously on any machine or CI runner already running `umask 077` —
  the file would be 0600 whether or not `setOwnerOnlyPerms()` ran, and the rule
  under test would not be the rule making the fixture pass. Under `022` the
  file would be 0644 by default, so 0600 can only come from the call.
  <br>**The store file only.** SQLite removes `-wal` and
  `-shm` when the last connection closes cleanly, and `run()` closes its
  connection before returning — verified 2026-08-06: a WAL database written and
  closed leaves `t.db` alone on disk, no sidecars — so a post-call
  `QFile::permissions()` on them would stat files that no longer exist and pass
  or red for a reason unrelated to the rule. The sidecars *are* chmodded, by
  `RoadmapStore::open()`, and ANTS-3756 INV-17 is where that is asserted;
  re-asserting it here would duplicate another spec's invariant with a fixture
  that cannot see it.
- **INV-10** — `notes[]` honours both bounds in § 2.4. *Test:* feature test —
  a fixture roadmap carrying >200 note-raising lines yields exactly 200
  entries, `notes_truncated: true`, and a `notes_count` equal to the true
  total (>200); a fixture whose note `detail` would exceed 2 KiB yields a
  `detail` of exactly 2 KiB ending in the ellipsis. Both legs are needed
  because the two bounds are independent — capping entries bounds no bytes.

## 4. RAM / build cost

**Memory.** One `Access::Bulk` connection's **16 MiB** page cache
(`kBulkCacheKiB`, ANTS-3756 § 2.5) plus one plan, for the duration of one
call, then released (§ 2.2 owns why the connection is per-call).

**This is a deliberate deviation from ANTS-3765 § 4's shape, and peak cost is
unchanged by it.** That spec takes an *open* store precisely so "ten projects
migrated through one connection cost one 16 MiB cache, not ten" — advice aimed
at a batch driver looping over a corpus. This verb migrates **one project per
call**, and its connections are strictly sequential rather than concurrent, so
peak residency is still one cache; what a per-call open costs is time (below),
not memory. Named here so nobody "fixes" the verb to take an open store and
hands it a lifetime problem instead. ANTS-3757 § 4
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
129 ms is the load's cost, not the verb's.

**So the 1 s figure is a budget this spec does not assert, and says so rather
than implying a contract.** No invariant carries a latency surface: asserting a
wall-clock bound needs the verb's own measurement, which does not exist yet,
and an invariant whose number was borrowed from a different call shape is one
that reds for the wrong reason. **The verb's end-to-end figure is measured at
implementation and folded back here** (`/write-spec` Step 8); if it lands near
the ceiling, a latency invariant is added with it, on ANTS-3793's
`Inv3Latency` model. Until then the ceiling is the design target and the
129 ms is its provenance.

**Build.** One new `.cpp` and its header in an existing library. No new target, no new
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
- **Re-rooting a migrated project.** A project whose directory moves presents a
  new canonical root, so step 6 sees its slug owned by a different root and
  refuses `slug_collision` with no remedy this verb offers. Deferred rather
  than excluded — it will be wanted the first time someone moves a repo — and
  filed as **ANTS-3857**. The workaround until then is to pass a fresh
  `export_slug`, which migrates the moved tree as a new project.

## 6. Tests

Feature test: `tests/features/roadmap_migrate_verb/`. Covers INV-1..INV-10.
Label `features;fast`. Source added to `test_core`'s `SOURCES` list — not
`add_executable` (`tests/features/README.md`).

**One testing route, and § 2.1.1 is what provides it: every behavioural
invariant drives `RoadmapMigrateVerb::run(storePath, req)` directly**, with
`storePath` inside a `QTemporaryDir`. The registered handler
(`cmdRoadmapMigrate`) is *not* driven by any test — its whole remaining job is
to resolve `RoadmapStore::defaultPath()`, for the reason § 2.1.1 gives — and
that one line is covered by INV-2's source-grep leg instead.

| Invariant | Route |
|---|---|
| INV-1 | source-grep only |
| INV-2 | (a) `run()` against a temp store · (b) source-grep |
| INV-3..INV-10 | `run()` against a temp store |

Each invariant is verified to FAIL against pre-change source before the code
is restored, per the project test convention. INV-1 and INV-2(b) fail
trivially (no such TU); INV-2(a) and INV-3..INV-10 fail to compile against a
tree with no `RoadmapMigrateVerb::run()`, which is the must-fail-first proof
for a new surface.

Fixture: a `QTemporaryDir` project root carrying a small hand-written
`ants-v1` `ROADMAP.md`, canonicalised before it is passed (§ 2.3's precondition,
which INV-7 and INV-8 both rest on), plus a store path inside that same temp
dir — created by the verb, not by the fixture, so INV-6 and INV-9 can assert on
what creation does.

**Staging the must-fail-first proof.** INV-2(a) and INV-3..INV-10 do not
compile against a tree with no `RoadmapMigrateVerb::run()`, and a compile
failure takes the whole `test_core` bundle down — so INV-1 and INV-2(b) could
not be observed failing independently in that same tree. The proof is therefore
staged: first add the header with a declaration only and the two source-grep
legs, and watch them red against a tree with no handler TU; then add the
remaining legs against the declared-but-unimplemented seam.

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
| 3 (cap) | 2026-08-06 | 3 cold `general-purpose`, same packet rebuilt, no prior-loop briefing | C 1 · H 4 · M 8 · L 11 — **24 verified, 4 dismissed** | All 24 fixed; **converged by cap, nothing deferred**. Dimension tally: dim 15×8, dim 4×5, dim 7×5, dim 6×3, dim 5×2, dim 1×1, dim 9×1, dim 10×1, dim 12×1, dim 13×1. **Origin: almost entirely loop 2's own collateral** — the second consecutive collateral-dominant loop, which is why the run stops at the cap rather than asking for a fourth. The CRITICAL is the clearest case: loop 2 added two refusals that *presuppose existing rows* (a re-run changing this root's slug or name), which made INV-4's "the store holds no `project` row" false for a correct implementation — restated as "no refusal **adds or changes** a row", with the two handler-level refusals `run()` cannot reach named as covered by inspection. Three cross-references pointed at invariants saying the opposite of what cited them: § 2.3.1 said "INV-6 is stated against rows" when INV-6 is deliberately about file existence and INV-4 is the row one; § 2.2 said "INV-8 locks it" of a property loop 2 had just removed from INV-8; § 2.5's table filed the re-run name check under step 1 when the sequence performs it at step 6, where the row it reads exists. All three lanes independently found the **stamp origin** contradiction — § 2.3 said "taken once, at step 7" while step 0b passes a stamp *into* `run()` — which would have put the clock read back inside `run()` and destroyed the reproducibility the seam exists for; the stamp now has its own § 2.3.2 and the handler owns the read. Two more fixtures could not have run as written: **INV-5**'s `history_rows > 0` guard is unreachable on a first-ever load, because `Loader::recordHistory()` is reached only from the field-update path (`src/roadmapmigrateload.cpp`) — the fixture is now a re-run over an edited source; and **INV-9** asserted 0600 without controlling the umask, so it passed vacuously under `umask 077` — it now sets `umask(022)` for the call. Also closed: step 6's two lookups conflated "no row" with an SQL error (now `store_failed`, the split `migratedProject()` already makes); INV-1's "`//`-leading" skip would have redded against the two *indented* member comments that actually carry the mentions; the `notes[]` caps shipped with no invariant (now INV-10); `run()`'s connection teardown was load-bearing for INV-9 and unstated (`~RoadmapStore()` does `removeDatabase()`); and § 4 never named its deliberate departure from ANTS-3765 § 4's one-connection-for-ten-projects shape. **Dismissed on verification:** the "4 MiB" comment in `roadmapstore.cpp` for the **third** time (it is ANTS-3761 INV-12's export RSS budget, not the cache size — recorded here so a fourth run does not spend on it); `kDefaultHistoryCapBytes` unconfirmed (`250LL * 1024 * 1024`, `src/roadmapstore.h:23`); `auditautofix.cpp` unverified (opened, it ships the expression); whether `mcp-tools.md` carries a `dry_run` support list (it does, and § 7's edit has a target). **New id filed rather than left as a promise:** ANTS-3857, re-rooting a moved project. Doc 653 → 740 lines. **Recommendation on exit: split at § 2's seams before implementation if this spec is revisited** — three loops of lanes have flagged its length, and roughly a third of the body is why-this-shape justification rather than contract. |
| 2 | 2026-08-06 | 3 cold `general-purpose`, same packet rebuilt against the edited doc, no prior-loop briefing | C 0 · H 4 · M 7 · L 13 — **24 verified, 3 dismissed** | All 24 fixed. Dimension tally: dim 15×7, dim 5×6, dim 4×5, dim 6×3, dim 1×2, dim 9×2, dim 10×1, dim 13×1, dim 2×1. **Zero CRITICAL — loop 1's structural defects did not resurface.** **Origin split: ~14 fix collateral vs ~5 draft defects**, which is Phase 5's collateral-dominance trigger; answered with a consolidation sweep rather than only reconciliation — § 6's restatement of § 2.1.1's ANTS-3856 rationale cut to a pointer, INV-4's restatement of § 2.5's WAL wording cut, and an unbacked "small enough not to need the offload path" claim deleted rather than given a number it did not have. Two fixtures could not have run: **INV-9** checked `-wal`/`-shm` permissions *after* `run()` returns, but SQLite removes both on a clean last-connection close (verified by writing and closing a WAL db — only `t.db` survives), so it is narrowed to the store file, the sidecar half left to ANTS-3756 INV-17 which already owns it; **INV-8** claimed the no-restart property but tested `migratedProject()`, which never touches the `roadmapStoreOrNull()` cache where that property lives — narrowed to what the fixture falsifies, with the stronger claim attributed to ANTS-3793. Loop 1's own step 0a/0b was **wrong**: `CallerCwdContract::Required` already refuses an empty `caller_cwd` before the handler runs, and `ResolvedRoot::Source` supplies the discriminator loop 1 said had to be checked by hand — rewritten against `src/resolvedroot.h`, which also supplied the unstated canonical-root precondition three later steps rest on (it uses the same `QFileInfo::canonicalFilePath()` as `migratedProject()` and `registerProject()`, so the forms agree — but silently). Also closed: step 6 was missing the re-run slug-change condition its own prose required; the symmetric `project_name` re-run case was undecided; whether a caller-supplied `export_slug` is slugified or validated verbatim was ambiguous, and INV-6 tested nothing under one reading; the `notes[]` cap bounded entries but not bytes (`Note::detail` is an unbounded `QString`) — now 200 entries × 2 KiB; concurrent lock contention had no stated outcome. **Dismissed on verification:** INV-8's three-argument `migratedProject()` call "would not compile" (both trailing params default to `nullptr` in `src/roadmapsource.h`); the `**Layman:**` line's placement (blank-line separated from the header block, matching ANTS-3766, and `spec_query` parses); no-TOC, dismissed a second time on the same evidence — 0 of 4 sibling specs carry one. **Corrected mid-fix by verification, not by a lane:** this loop's own fix first named a `ANTS_SOURCE_DIR` define for INV-1's source scan; the define that exists on `test_core` is `ANTS_SRC_DIR` (`CMakeLists.txt:2485`), and INV-1 now names that one. Doc 576 → 650 lines despite the consolidation — the growth is contract, but § 5.3's yardstick is now the live concern and loop 3 is the last. |
| 1 | 2026-08-06 | 3 cold `general-purpose`, one shared byte-identical packet | C 3 · H 4 · M 6 · L 10 · I 1 — **23 verified, 3 dismissed** | All 23 fixed. Dimension tally: dim 15×5, dim 5×5, dim 4×3, dim 7×3, dim 10×3, dim 1×2, dim 6×2, dim 9×2, dim 12×2, dim 8×1, dim 13×1. **All three lanes independently led on the same defect**, which is what makes the tail credible: § 6 said INV-2..INV-8 were "driven through the registered handler" and then that the handler resolves `defaultPath()` internally, so six invariants had no runnable test surface and an implementer following ¶1 would have migrated into the user's real store — reproducing ANTS-3856. Fixed by § 2.1.1's `RoadmapMigrateVerb::run(storePath, req)` seam, on the precedent ANTS-3793 § 2.2 set for `storeFor()`. Two more the draft got flatly wrong: INV-4 claimed the store is "byte-unchanged" after *every* refusal, false for the two that follow `store.open()` (which creates the file and both WAL sidecars) — restated at row level; and `dry_run` was never reconciled with `mcp-tools.md`'s "returns before any disk write", which it violates by opening the store — now § 2.3.1, stated as a bounded deviation with the argument for why a throwaway store would make the preview *wrong* rather than merely different. Also added: a `slug_collision` pre-check (`export_slug` is `UNIQUE` and the default is derived, so two roots slugifying alike collided into the catch-all), § 2.6's trust boundary + INV-9 (`specs.md` § 5.4 requires it and the draft had none), a 200-entry `notes[]` cap, and the 1 s/project ceiling ANTS-3765 § 4 sets — the draft had quoted only the 60 s bridge timeout, 60× looser. **Dismissed on verification:** no-TOC (no sibling spec carries one and `specs.md` § 3's required order omits it); a claimed-stale "4 MiB" comment in `roadmapstore.cpp` (lane misread — the 4 MiB is ANTS-3761 INV-12's export RSS budget, not the cache size); `features;fast` not being a real label (it is — `CMakeLists.txt:1001`). **Found during packet construction, before a lane was spent:** nothing. **Found by 4b's sweep, not by a lane:** the `**Layman:**` line still promised a preview that writes nothing, contradicting the § 2.3.1 just added, and two sub-subsections used `###` where siblings use `####`. **Executed rather than read** (4a step 2): the `export_slug` `CHECK` against real SQLite (`Ants_Terminal` fails both clauses, `ants-terminal` passes both, empty fails — so INV-6's fixture is valid), and `QDateTime::currentDateTimeUtc().toString(Qt::ISODate)` compiled against Qt 6, returning `2026-08-06T20:22:28Z`, which `isIsoZStamp()`'s regex accepts. Doc grew 318 → 575 lines; watch it, and split at § 2's seams if loop 2 is still finding structural defects. |
