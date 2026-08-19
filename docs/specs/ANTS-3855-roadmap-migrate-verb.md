# ANTS-3855 — Add `roadmap_migrate`, the verb that loads a project into the store

**Status:** accepted (2026-08-06) — cold-eyes loops 1–3, converged by cap, no deferred findings. **Amended 2026-08-19** — § 2.4's envelope and § 3's invariants, for the four items **Covers:** names; cold-eyes loops 4–5, capped, 16 verified and 16 fixed.
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3855 (in-session-2026-08-06, measured while starting ANTS-3853's first item).
**Blocker for:** ANTS-3807 (per-project migration briefs), ANTS-3772, ANTS-3815.
**Composes with:** ANTS-3757 (read half), ANTS-3765 (load half), ANTS-3793 (consumer cutover).
**Covers:** ANTS-4478, ANTS-4479, ANTS-4482 (envelope half), ANTS-4490 (envelope half) — four cross-session reports that each want a different field of one enumerated envelope, so they share one contract rather than four documents that must agree forever.

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
`remotecontrol_roadmap_log.cpp`).

**TWO TUs, not one — corrected at implementation (2026-08-06) against a link
failure, not a preference.** § 2.1.1's seam lives in its own
`src/roadmapmigrateverb.{h,cpp}`, outside `ANTS_RC_SOURCES`. A static archive
is pulled in at OBJECT granularity, so a seam sharing an object file with the
handler drags `RemoteControl` → `ants::resolveCallerCwdRoot` → `MainWindow`
into every bundle that links it — and `test_core` links `ants_core_lib`
**alone**, with no `ants_chrome_lib` (`CMakeLists.txt`'s
`_ants_subset_linked_libs`, the same reason those libs are held out of unity
builds). The one-TU draft failed `test_core`'s link with ~20 undefined
`MainWindow` / `ClaudeIntegration` / `AuditEngine` symbols. A seam that cannot
be linked apart from the thing it exists to be tested apart from is not a seam,
so the split is what § 2.1.1's own argument requires rather than a departure
from it.

**No new link edge**: `ants_core_lib`
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
of substance. The work is a free function in its own TU (§ 2.1's correction):

```cpp
// src/roadmapmigrateverb.h — the testable seam.
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
 9. envelope from `out`, plus `project_id` from step 6's `owner` when it is set
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
  "store_backed": true, "markdown_rewritten": false,
  "items_inserted": 0, "items_updated": 0, "items_updated_governed": 0,
  "items_unchanged": 0, "items_orphaned": 0, "ids_allocated": 0,
  "sections_written": 0, "sections_unchanged": 0,
  "elements_written": 0, "history_rows": 0,
  "updated_items": [], "updated_items_truncated": false,
  "defaulted_fields": {},
  "notes": [], "notes_count": 0, "notes_truncated": false
}
```

The example is the **shape**, not a measurement — every numeric field is
zeroed and `notes[]`, `updated_items[]` and `defaulted_fields` are empty, so no
figure here can be read as this project's, `project_id` included. § 4 carries
the measured ones.

**`items_updated_governed` and `defaulted_fields` arrived with ANTS-4065 § 2.6
and were shipped without being enumerated here**; this amendment adds them to
the block rather than leaving two live fields undocumented in the one place that
claims to enumerate the envelope. Neither behaviour changes.

`sources[].path` is **relative to `req.projectRoot`** — the canonical root of
§ 2.3 — so `ROADMAP.md` and `docs/roadmap/0.6.md`, not the absolute paths
`findRoadmaps()` resolved. The caller supplied that root; echoing it back on
every entry is noise.

`store_backed` answers the question the counts cannot: **will `roadmap_query`
and `roadmap_log` serve this project from the store after this call?** It is
`plan.sources[0].format == "ants-v1"` — index 0 is the live roadmap, and it is
what `project.source_format` records (`src/roadmapmigrate.h`, the sole
precondition ANTS-3815 INV-2 rests on) — because `RoadmapSource::migratedProject()`
returns `nullopt` for every other dialect, by design and with a comment saying
so ("legitimately markdown-served", `src/roadmapsource.cpp`). A
`github-task-list` or `pass-headings` project therefore migrates `ok: true` with
faithful counts and is still answered from markdown.

**Under `dry_run` it is still the format answer**, and reads as *would this
project be served from the store if this run committed*. It does **not** go
`false` merely because nothing was committed. This is deliberately the opposite
posture to `project_id` two paragraphs up, and the two differ because their
subjects do: an id is a row that either exists or does not, and a rolled-back
row does not exist — a dialect is a property of the file on disk, which a
rollback cannot change. A preview whose value flipped on commit would answer
nothing a caller could act on before committing, which is what a preview is for.

**Added 2026-08-19 (ANTS-4490).** Vestige migrated 1026 items, got a healthy
envelope, and kept being served from markdown; the only tell was the ABSENCE of
unrelated fields on later `roadmap_query` responses, which no caller notices.
The verb's description now says this in prose, which a script cannot read — the
field is what makes the outcome legible to one.

`markdown_rewritten` is **always `false`**, and it is a field rather than a
sentence for the same reason. This verb reads the roadmap and never writes it:
`ROADMAP.md` is byte-identical after a successful migration and `git status` is
clean. The first re-render is the next `roadmap_log` write, which reports
`files_written` naming it, and reflows the whole file. INV-11 pins both halves
— the constant and the property that makes it true.

**Added 2026-08-19 (ANTS-4482).** Three sessions verified the byte-identity with
checksums and read it as a migration that had not run; on a project whose
`store_high_water` is also 0, both available signals point at failure after a
clean success. A constant is the right shape here: INV-11 is what makes the
value true, so a caller reading the field never has to know which release
changed it.

`updated_items[]` names the items `items_updated` counted, each with the fields
that changed:

```json
"updated_items": [{"id": "ANTS-1234", "fields": ["status", "body"]}]
```

`items_updated` stays the true total, so the array needs no count of its own. It
is bounded at **200 entries**, on `notes[]`'s pattern and for its reason, with
`updated_items_truncated` on breach; the cap is applied where the entries are
collected, so nothing unbounded accumulates in `Outcome`. `fields[]` carries the
store's column names, in the order the load wrote them.

`id` is the **stored** id — `it.id.isEmpty() ? cur->id : it.id`, the form
`Loader::applyPlanFields()`'s `field_conflict` note already uses. A matched item
whose source bullet carries no id is a real state (§ 2.6.1's re-match pairs by
headline and order), so collecting the plan's id alone would emit an entry with
an empty `id` that no caller can act on and no test can match.

**Added 2026-08-19 (ANTS-4479).** A dry run reporting `items_updated: 3` gave no
ids and no fields, so the reporter could not tell a reconciliation of real drift
from a lossy re-parse flattening good rows — and backed `ROADMAP.md` up to a
scratchpad and diffed afterwards to prove it was safe. Under `dry_run` this
turns a count into a reviewable plan, which is the whole value of a preview.

`sections_unchanged` is `sections_written`'s partner, and exists because
`sections_written: 0` alone is illegible: it is INV-7's proof of idempotence and
reads as a counter that never moved. Both count sections the plan carried —
`written` those inserted or differing, `unchanged` those matched with nothing to
write — so their sum is the plan's section count on every run, and
`0 written / 236 unchanged` says what happened where `0` did not.

**Added 2026-08-19 (ANTS-4490).** Vestige reported `sections_written: 0` "on
every run despite 236 section rows existing" as a bug in its own right. It was
not one: the counter was correct and unreadable.

`project_id` is the store's `project_id` for this root, on **both** paths
whenever a row for this root already exists — step 6 has read it already
(`readProjectByRoot()`, before the transaction opens), and that id is durable,
pre-existing, and the very thing the same envelope's `items_updated` /
`items_unchanged` counts were diffed against.

**So `0` means one thing only: this root has no `project` row yet.** Under
`dry_run` that is the truthful answer — the rowid `registerProject()` would
allocate is inside a transaction about to roll back, and a provisional id a
later real run need not reuse is worse than no id, because it looks durable. A
committed run always reports non-zero. A caller scripting "migrate only if not
already present" therefore reads `project_id > 0` on a dry run as "already
migrated", which is the question it was asking.

**Amended 2026-08-19 (ANTS-4478).** This read "Under `dry_run` it is `0`"
unqualified, and INV-3 tested that by name. Three projects reported the same
shape independently: a dry run over an already-migrated project answered
`project_id: 0` beside counts that could only have been computed against that
project's real rows, so the envelope proved the lookup succeeded while the id
said it had not. The reason the old rule gave is sound and was over-general — it
holds for a project the run is REGISTERING, and for no other. **Omitting the
field when it cannot be resolved was considered and rejected:** 0 is unambiguous
once the pre-existing case is handled, and an absent key costs every caller a
second branch to tell it from a zero.

`notes[]` is bounded on **both axes**, because capping the element count alone
bounds no bytes — `Note::detail` is a `QString` with no length rule of its own:

| Bound | Value | On breach |
|---|---|---|
| entries | 200 | `notes_truncated: true`; `notes_count` stays the TRUE total |
| `detail` | 2048 characters each | that entry's `detail` is clipped to exactly 2048 with an ellipsis as the last character |

**The detail bound is in CHARACTERS, not bytes** — corrected at implementation
(2026-08-06) from an earlier "2 KiB", because a byte bound is not assertable
against a `QString` without a re-encode on every note, and characters is the
unit the rest of this project's note handling already uses
(`rcdetail::kRcMaxNoteChars`). For the ASCII-dominant details these notes carry
the two are the same number; for a note quoting non-Latin source text the array
can reach ~1.6 MiB rather than ~400 KiB in the pathological worst case, which
is still bounded and still ~200× smaller than the roadmap it came from.

So the array is ≤ ~400 KiB for ASCII input and typically a few KiB. The cap
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

**Two of the fields above are new members of `Outcome`, added by this amendment
on ANTS-4065 § 2.6's precedent** — that spec added `itemsUpdatedGoverned` the
same way, without reopening ANTS-3765's declaration block, and this document
records the extension in § 7 rather than editing that one. They are
`QVector<UpdatedItem> updatedItems` (`{QString id; QStringList fields;}`, capped
at 200) and `int sectionsUnchanged`. **The load is the only layer that can know
either**: `Loader::applyPlanFields()` already holds each changed `f.column` and
the item's id, and `Loader::matchSections()` already distinguishes a section it
wrote from one it matched — so both are collected where the decision is made,
never re-derived by the verb.

**`updated_items_truncated` is not a third member.** The verb derives it from
what it was handed — true exactly when `items_updated` exceeds the array's
length — and a stored bool would be a second answer to a question those two
values already settle.

**Only `sectionsUnchanged` joins ANTS-3765 INV-13's dry-run/real-run
comparison**, which compares the two `Outcome`s "count by count, excluding
`projectId`". `updatedItems` is not a count and that invariant's wording does
not reach it, so **its parity is asserted here instead**, at the envelope, by
INV-3 — which is where this document's contract lives, and costs ANTS-3765 no
wording change.

**Four envelope values do not come from `Outcome`, and none of them is a
tally.** `store_backed` is read off `plan.sources[0]`; `markdown_rewritten` is
a constant INV-11 makes true; `defaulted_fields` is `defaultedFieldTally(plan)`;
and `project_id` is step 6's `owner` where that row exists, which is why § 2.3
names it at step 9 rather than leaving the sequence reading `envelope from
`out``. The no-recompute rule binds the counts, and every count still comes from
`Outcome` untouched.

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
  `src/roadmapmigrateverb.cpp` — the SEAM's TU per § 2.1's correction, not the
  handler's — and no other file under `src/`.
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
  all three files of the verb — `src/roadmapmigrateverb.{h,cpp}` and
  `src/remotecontrol_roadmap_migrate.cpp` — under INV-1's comment-skipping rule,
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
  through the verb), `updated_items` included. Also asserts § 2.4's dry-run
  envelope rules: `sources[].path` is relative on both (`ROADMAP.md`, never an
  absolute path), and **`project_id` is `0` on a dry run over a root with no
  `project` row and equal to the real id on a dry run over one already
  migrated**.
  <br>**The project_id clause needs three runs, not two**, because one dry run
  cannot exhibit both cases: dry over an empty store (expect `0`), the real run
  (expect non-zero), then a second dry run over the now-migrated root (expect
  the *same* id the real run reported). The third is the leg three projects
  reported, and the one the pre-amendment code **fails**: it answers `0` where
  the real id is expected.
  <br>**A FOURTH run carries the `updated_items` and `sections_unchanged`
  parity, because none of the first three can.** Each of them runs over a store
  with no prior rows or over an unchanged root, so `items_updated` is `0` and
  `updated_items` is `[]` on both sides of every comparison — and two empty
  arrays agreeing is not evidence. On INV-5's pattern, and for INV-5's reason:
  migrate, edit two bullets in the fixture, then run a dry and a real pass over
  the **edited** source and compare those two envelopes. That pair is the only
  one in which either value is non-empty, and an implementation collecting
  `updatedItems` on the committed path alone passes every other leg.
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
  <br>**`sections_unchanged` is asserted on that same second run** and equals the
  number of sections the fixture carries. It is what makes `sections_written: 0`
  readable as idempotence rather than as a counter that never moved (§ 2.4), and
  asserting it here rather than in an invariant of its own is deliberate: the two
  figures are one statement about one run.
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
  total (>200); a fixture whose note `detail` would exceed 2048 characters
  yields a `detail` of exactly 2048 ending in the ellipsis. Both legs are needed
  because the two bounds are independent — capping entries bounds no bytes.
- **INV-11** — This verb writes no **source** file under `req.projectRoot`, and
  `markdown_rewritten` says so. *Test:* feature test — hash every file under the
  fixture root before `run()` and again after a successful **non-dry** run;
  assert every pre-existing file's hash is unchanged (`ROADMAP.md` included) and
  that the **only** new path is the store, then assert `markdown_rewritten` is
  `false` on **both** envelopes — the dry run is a second call, not a second
  assertion on the first.
  <br>**Not "every hash unchanged", because § 6's fixture puts the store under
  that same root and the verb creates it** — the flat form reds against a correct
  implementation, on the one file the fixture REQUIRES the verb to write.
  Excluding `*.sqlite*` from the hash set instead would hide a regression in the
  store's own location, which is what INV-6 and INV-9 rest on. The `-wal` /
  `-shm` sidecars need no exclusion: INV-9 establishes that SQLite removes both
  when `run()` closes its connection, so neither exists when the test re-hashes.
  <br>*Breaks when:* a later change renders at migrate time — which is a live
  proposal (ANTS-4483), and this is where it should land as a red test rather
  than as an unexplained whole-file reflow in somebody else's commit.
- **INV-12** — `store_backed` agrees with the consumer dispatch **on a committed
  run**, and stays the format answer on a dry one. *Test:* feature test, **three
  legs** across two fixture roots. (a) an `ants-v1` roadmap:
  `store_backed` is `true` and `RoadmapSource::migratedProject()` returns the
  project id afterwards. (b) a `github-task-list` roadmap: `ok` is still `true`
  with non-zero counts, `store_backed` is `false`, and `migratedProject()`
  returns `nullopt`. (c) both roots again under `dry_run:true`, asserting
  `store_backed` **alone** — `true` for the `ants-v1` root, `false` for the
  other.
  <br>**Leg (c) deliberately does not assert `migratedProject()`.** Nothing
  committed, so it is `nullopt` for both roots while `store_backed` stays `true`
  for the first: the two disagree on purpose (§ 2.4), and a leg written as "the
  same values as (a) and (b)" would red against a correct implementation.
  <br>**Leg (b) is the one that matters** — it pins that a *successful*
  migration whose project is not store-backed says so in the envelope, which is
  the state Vestige could only detect by noticing which fields a later
  `roadmap_query` response did not carry. **Leg (c) pins the dry-run posture**
  § 2.4 states, and it is the one an implementer is most likely to build the
  other way.
- **INV-13** — `updated_items` names exactly the items `items_updated` counted,
  with the fields that changed. *Test:* feature test — migrate a fixture, edit
  two bullets in the source (one `status`, one `headline`), migrate again, then
  assert `items_updated == 2`, that `updated_items` carries those two ids and no
  others, and that each entry's `fields` is exactly the one column changed.
  <br>**A second leg pins the bound**, on INV-10's pattern: a fixture whose
  re-run changes more than 200 items yields exactly 200 entries,
  `updated_items_truncated: true`, and an `items_updated` carrying the true
  total. One bound rather than `notes[]`'s two — an entry here is an id and a
  short column list, so capping the entries bounds the bytes.
  <br>*Breaks when:* the array is filled from the plan rather than from the write
  path — every matched item would then appear, and the array would say nothing
  the count does not.

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

**Build.** Two new `.cpp` files and one header in an existing library — the
seam and the handler, split for the link-time reason § 2.1 records. No new
target, no new dependency, no new link edge (§ 2.1). The feature test joins `test_core`'s
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
  ANTS-3816. **The schema-upgrade path** —
  [ANTS-3781](ANTS-3781-roadmap-store-schema-upgrade.md), which this spec does
  not need because it adds no column.
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

Feature test: `tests/features/roadmap_migrate_verb/`. Covers INV-1..INV-13.
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
| INV-3..INV-13 | `run()` against a temp store |

Each invariant is verified to FAIL against pre-change source before the code
is restored, per the project test convention. INV-1 and INV-2(b) fail
trivially (no such TU); INV-2(a) and INV-3..INV-10 fail to compile against a
tree with no `RoadmapMigrateVerb::run()`, which is the must-fail-first proof
for a new surface.

**Every leg the 2026-08-19 amendment adds is proved red the same way, against
the shipped code rather than an empty tree — named rather than counted, because
a count is one more thing to keep true.** INV-3's third run reds on the value
(`project_id: 0` where the real id is expected); INV-3's fourth run, INV-7's
`sections_unchanged` leg, INV-11, INV-12's three legs and INV-13's two red on an
absent field. A fixture root carrying a
`github-task-list` roadmap is **copied from the one already in the tree** —
`tests/features/roadmap_migrate_read/fixtures/archives/declaredformat/`, which
carries real bullets — rather than authored again; INV-12's legs (b) and (c) are
its only consumers, and a `roadmap_migrate_verb`-local copy keeps that
directory's own fixtures free to change. The `ants-v1` fixture already carries
sections, so INV-7's leg needs no new fixture; INV-3's fourth run edits it;
INV-13's second leg needs one that changes more than 200 items, which is
generated rather than hand-written.

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
- **[ANTS-3765](ANTS-3765-roadmap-migration-load.md)** — `Outcome` gains
  `updatedItems` and `sectionsUnchanged` (§ 2.4). Recorded here rather than by
  editing that spec's declaration block, on the precedent ANTS-4065 set when it
  added `itemsUpdatedGoverned` the same way. **Its INV-13 compares counts**, so
  `sectionsUnchanged` joins it and the load's own feature test gains that field
  where it compares a dry run against the real one. `updatedItems` is not a
  count and stays outside it — INV-3 asserts that array's parity at the
  envelope instead — so no wording change is owed to that invariant.
- **The verb's `tools/list` description** gains the five new response fields,
  named rather than counted: `store_backed`, `markdown_rewritten`,
  `sections_unchanged`, `updated_items` and `updated_items_truncated`.
  ANTS-4482 and ANTS-4490 put the same facts there in prose on 2026-08-18
  (commit 591c1c52); the fields are what a caller can branch on, and the
  description now names them.
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
| 5 (cap) | 2026-08-19 | 3 cold `review-lane`, identical brief, packet and scrubbed copy rebuilt from disk after loop 4's fixes | **Q1 2 · Q2 5 · Q3 0 · Q4 1** — verified 8, dismissed 1 | **Eight verified, eight fixed. Cap reached (2 loops for a spec); the run ships and implementation is the third reviewer.** **A high-collateral cap, and saying so is the point: all eight landed on text THIS RUN wrote, and six on text loop 4 added** — the run was repairing its own repairs, not converging on a settled document. What makes shipping right anyway is the split § At the cap states: a spec is *implemented* next, and the build tests the contract against real code in a way a third cold read cannot. **All three lanes independently found the same [Q4], and it is the sharpest finding of either loop.** Loop 4 routed `updated_items`' dry-run/real-run parity to INV-3 — and INV-3's three runs are all over a store with no prior rows or an unchanged root, so `items_updated` is `0` and `updated_items` is `[]` on **both** sides of every comparison. Two empty arrays agreeing is not evidence, and an implementation collecting `updatedItems` on the committed path alone would have passed every leg while shipping the empty preview ANTS-4479 exists to prevent. INV-3 gains a fourth run over an EDITED source, on INV-5's pattern and for INV-5's stated reason — the identical trap, one invariant along, which loop 4 read past. **Loop 4's INV-12 leg (c) was wrong twice in one clause:** it was added under a `*Test:*` line still reading "two legs", and it said the two roots report "the same two values" when `store_backed` and `migratedProject()` **disagree on purpose** under `dry_run` — so the leg as written reds against a correct implementation, and an implementer building to the stated count drops it entirely. Now three legs, with (c) asserting `store_backed` alone and saying why. **The `project_id` rule had no home in the sequence:** § 2.3's unamended step 9 still read `envelope from `out``, and § 2.4 called `store_backed` and `markdown_rewritten` "the two exceptions" — so an implementer following either would have written back the pre-amendment `req.dryRun ? 0 : out.projectId`, the exact line INV-3's third run exists to red on. Step 9 now names `owner`, and the exceptions are four, `defaulted_fields` included. **Two Q1s, one of them the orchestrator's own:** § 6 called the `github-task-list` fixture "new" when `tests/features/roadmap_migrate_read/fixtures/archives/declaredformat/` already ships one with live bullets (two lanes), and loop 4's claim that the next `roadmap_log` write reports `files_written: ["ROADMAP.md"]` was disproved by running one during this session — it names the archives too. **Counts replaced by names in both places a lane caught one drifting** (§ 7's "four new response fields" against five, § 6's "five legs"), which is the same rule this skill applies to itself. **Dismissed on materiality, for the second consecutive loop:** `defaulted_fields` attributed to ANTS-4065 § 2.6 — recorded here so a later run does not spend on it again. **Two open questions resolved clean by reading source rather than by a lane:** `fieldsOf()` compares nine columns and neither `provenance` nor a headline edit moves a second one, so INV-13's "exactly the one column changed" holds; and `matchSections()` has no `continue` before its `found` test, so `sections_written + sections_unchanged` really is the plan's section count. Doc 988 → 1017 lines — **the growth is the concern the 2026-08-06 run flagged twice, and its recommendation to split at § 2's seams is now three runs old.** |
| 4 | 2026-08-19 | 3 cold `review-lane`, one byte-stable shared-context file, scrubbed doc copy, packet carrying 13 verbatim source windows | **Q1 1 · Q2 3 · Q3 3 · Q4 1** — verified 8, dismissed 1 | **Eight verified, eight fixed. First loop of a NEW run, gating the 2026-08-19 envelope amendment** (§ 2.4 + § 3, for ANTS-4478 / 4479 / 4482 / 4490); the 2026-08-06 run closed at its cap on loop 3. Q-counts, not the retired C/H/M/L/I scale. **All three lanes independently found the same three defects**, which is the strongest signal any loop of this document has produced, and every one of them landed on text this amendment ADDED. **The worst is INV-11**: it told the implementer to "hash every file under the fixture root … assert every hash unchanged", while § 6's fixture puts the store "inside that same temp dir" and has the verb create it — so the new invariant reds against a correct implementation, on the one file the fixture REQUIRES the verb to write. Restated as *no pre-existing file changed and the only new path is the store*, with the `*.sqlite*` exclusion refused by name because it would hide a regression in the store's own location. **The Q1 was a false claim about a sibling spec**: § 2.4 and § 7 both said ANTS-3765 INV-13 is "stated over the whole `Outcome`", and that invariant reads "count by count, excluding `projectId`" — so `updatedItems`, a `QVector`, joined nothing and its dry-run parity was asserted nowhere. Now: `sectionsUnchanged` joins INV-13 as a count, `updatedItems` is asserted at the envelope by INV-3, and ANTS-3765 needs no wording change. **The third was `store_backed`'s dry-run value**, where the field's stated QUESTION ("will `roadmap_query` … serve this project from the store after this call?") and its stated FORMULA (`plan.sources[0].format == "ants-v1"`) answered differently under `dry_run`, one paragraph after the amendment settled exactly that shape for `project_id`; two builders would have built `true` and `!dry_run && …`. Stated explicitly, with the asymmetry against `project_id` argued from their subjects — a rolled-back row does not exist, a dialect on disk is unchanged by a rollback — and pinned by a new INV-12 leg (c). **Three more, one per lane.** `updated_items[].id` did not say WHICH id, where `applyPlanFields()` already branches `it.id.isEmpty() ? cur->id : it.id`, so a matched id-less bullet would have emitted an unusable empty id. `updated_items_truncated` was named nowhere in the `Outcome` extension list nor in the two stated exceptions, so one builder would have added a third member and another derived it — now explicitly derived by the verb. And § 3 said the third `project_id` leg is "the one the pre-amendment code passes only by reporting `0`" while § 6 says it reds, which would have had an implementer weaken the assertion back to the defect ANTS-4478 reported. **Dismissed on materiality:** § 2.4 attributes `defaulted_fields` to ANTS-4065 § 2.6, which is `itemsUpdatedGoverned`'s section — true-but-inert provenance, and the lane that raised it said so itself. **Two lane open questions, and only one resolved clean:** INV-12 leg (b) needs a `github-task-list` roadmap to yield non-zero counts, and `tests/features/roadmap_migrate_read/fixtures/archives/declaredformat/` already carries one with live bullets; the second — that the `updated_items` bound had no invariant while `notes[]`'s equivalent has INV-10 — was real, and is the run's one **[Q4]**: a stated bound with no falsifiable surface. It became INV-13's second leg, counted rather than filed as an open question. Doc 942 → 988 lines. |
| 3 (cap) | 2026-08-06 | 3 cold `general-purpose`, same packet rebuilt, no prior-loop briefing | C 1 · H 4 · M 8 · L 11 — **24 verified, 4 dismissed** | All 24 fixed; **converged by cap, nothing deferred**. Dimension tally: dim 15×8, dim 4×5, dim 7×5, dim 6×3, dim 5×2, dim 1×1, dim 9×1, dim 10×1, dim 12×1, dim 13×1. **Origin: almost entirely loop 2's own collateral** — the second consecutive collateral-dominant loop, which is why the run stops at the cap rather than asking for a fourth. The CRITICAL is the clearest case: loop 2 added two refusals that *presuppose existing rows* (a re-run changing this root's slug or name), which made INV-4's "the store holds no `project` row" false for a correct implementation — restated as "no refusal **adds or changes** a row", with the two handler-level refusals `run()` cannot reach named as covered by inspection. Three cross-references pointed at invariants saying the opposite of what cited them: § 2.3.1 said "INV-6 is stated against rows" when INV-6 is deliberately about file existence and INV-4 is the row one; § 2.2 said "INV-8 locks it" of a property loop 2 had just removed from INV-8; § 2.5's table filed the re-run name check under step 1 when the sequence performs it at step 6, where the row it reads exists. All three lanes independently found the **stamp origin** contradiction — § 2.3 said "taken once, at step 7" while step 0b passes a stamp *into* `run()` — which would have put the clock read back inside `run()` and destroyed the reproducibility the seam exists for; the stamp now has its own § 2.3.2 and the handler owns the read. Two more fixtures could not have run as written: **INV-5**'s `history_rows > 0` guard is unreachable on a first-ever load, because `Loader::recordHistory()` is reached only from the field-update path (`src/roadmapmigrateload.cpp`) — the fixture is now a re-run over an edited source; and **INV-9** asserted 0600 without controlling the umask, so it passed vacuously under `umask 077` — it now sets `umask(022)` for the call. Also closed: step 6's two lookups conflated "no row" with an SQL error (now `store_failed`, the split `migratedProject()` already makes); INV-1's "`//`-leading" skip would have redded against the two *indented* member comments that actually carry the mentions; the `notes[]` caps shipped with no invariant (now INV-10); `run()`'s connection teardown was load-bearing for INV-9 and unstated (`~RoadmapStore()` does `removeDatabase()`); and § 4 never named its deliberate departure from ANTS-3765 § 4's one-connection-for-ten-projects shape. **Dismissed on verification:** the "4 MiB" comment in `roadmapstore.cpp` for the **third** time (it is ANTS-3761 INV-12's export RSS budget, not the cache size — recorded here so a fourth run does not spend on it); `kDefaultHistoryCapBytes` unconfirmed (`250LL * 1024 * 1024`, `src/roadmapstore.h:23`); `auditautofix.cpp` unverified (opened, it ships the expression); whether `mcp-tools.md` carries a `dry_run` support list (it does, and § 7's edit has a target). **New id filed rather than left as a promise:** ANTS-3857, re-rooting a moved project. Doc 653 → 740 lines. **Recommendation on exit: split at § 2's seams before implementation if this spec is revisited** — three loops of lanes have flagged its length, and roughly a third of the body is why-this-shape justification rather than contract. |
| 2 | 2026-08-06 | 3 cold `general-purpose`, same packet rebuilt against the edited doc, no prior-loop briefing | C 0 · H 4 · M 7 · L 13 — **24 verified, 3 dismissed** | All 24 fixed. Dimension tally: dim 15×7, dim 5×6, dim 4×5, dim 6×3, dim 1×2, dim 9×2, dim 10×1, dim 13×1, dim 2×1. **Zero CRITICAL — loop 1's structural defects did not resurface.** **Origin split: ~14 fix collateral vs ~5 draft defects**, which is Phase 5's collateral-dominance trigger; answered with a consolidation sweep rather than only reconciliation — § 6's restatement of § 2.1.1's ANTS-3856 rationale cut to a pointer, INV-4's restatement of § 2.5's WAL wording cut, and an unbacked "small enough not to need the offload path" claim deleted rather than given a number it did not have. Two fixtures could not have run: **INV-9** checked `-wal`/`-shm` permissions *after* `run()` returns, but SQLite removes both on a clean last-connection close (verified by writing and closing a WAL db — only `t.db` survives), so it is narrowed to the store file, the sidecar half left to ANTS-3756 INV-17 which already owns it; **INV-8** claimed the no-restart property but tested `migratedProject()`, which never touches the `roadmapStoreOrNull()` cache where that property lives — narrowed to what the fixture falsifies, with the stronger claim attributed to ANTS-3793. Loop 1's own step 0a/0b was **wrong**: `CallerCwdContract::Required` already refuses an empty `caller_cwd` before the handler runs, and `ResolvedRoot::Source` supplies the discriminator loop 1 said had to be checked by hand — rewritten against `src/resolvedroot.h`, which also supplied the unstated canonical-root precondition three later steps rest on (it uses the same `QFileInfo::canonicalFilePath()` as `migratedProject()` and `registerProject()`, so the forms agree — but silently). Also closed: step 6 was missing the re-run slug-change condition its own prose required; the symmetric `project_name` re-run case was undecided; whether a caller-supplied `export_slug` is slugified or validated verbatim was ambiguous, and INV-6 tested nothing under one reading; the `notes[]` cap bounded entries but not bytes (`Note::detail` is an unbounded `QString`) — now 200 entries × 2 KiB; concurrent lock contention had no stated outcome. **Dismissed on verification:** INV-8's three-argument `migratedProject()` call "would not compile" (both trailing params default to `nullptr` in `src/roadmapsource.h`); the `**Layman:**` line's placement (blank-line separated from the header block, matching ANTS-3766, and `spec_query` parses); no-TOC, dismissed a second time on the same evidence — 0 of 4 sibling specs carry one. **Corrected mid-fix by verification, not by a lane:** this loop's own fix first named a `ANTS_SOURCE_DIR` define for INV-1's source scan; the define that exists on `test_core` is `ANTS_SRC_DIR` (`CMakeLists.txt:2485`), and INV-1 now names that one. Doc 576 → 650 lines despite the consolidation — the growth is contract, but § 5.3's yardstick is now the live concern and loop 3 is the last. |
| 1 | 2026-08-06 | 3 cold `general-purpose`, one shared byte-identical packet | C 3 · H 4 · M 6 · L 10 · I 1 — **23 verified, 3 dismissed** | All 23 fixed. Dimension tally: dim 15×5, dim 5×5, dim 4×3, dim 7×3, dim 10×3, dim 1×2, dim 6×2, dim 9×2, dim 12×2, dim 8×1, dim 13×1. **All three lanes independently led on the same defect**, which is what makes the tail credible: § 6 said INV-2..INV-8 were "driven through the registered handler" and then that the handler resolves `defaultPath()` internally, so six invariants had no runnable test surface and an implementer following ¶1 would have migrated into the user's real store — reproducing ANTS-3856. Fixed by § 2.1.1's `RoadmapMigrateVerb::run(storePath, req)` seam, on the precedent ANTS-3793 § 2.2 set for `storeFor()`. Two more the draft got flatly wrong: INV-4 claimed the store is "byte-unchanged" after *every* refusal, false for the two that follow `store.open()` (which creates the file and both WAL sidecars) — restated at row level; and `dry_run` was never reconciled with `mcp-tools.md`'s "returns before any disk write", which it violates by opening the store — now § 2.3.1, stated as a bounded deviation with the argument for why a throwaway store would make the preview *wrong* rather than merely different. Also added: a `slug_collision` pre-check (`export_slug` is `UNIQUE` and the default is derived, so two roots slugifying alike collided into the catch-all), § 2.6's trust boundary + INV-9 (`specs.md` § 5.4 requires it and the draft had none), a 200-entry `notes[]` cap, and the 1 s/project ceiling ANTS-3765 § 4 sets — the draft had quoted only the 60 s bridge timeout, 60× looser. **Dismissed on verification:** no-TOC (no sibling spec carries one and `specs.md` § 3's required order omits it); a claimed-stale "4 MiB" comment in `roadmapstore.cpp` (lane misread — the 4 MiB is ANTS-3761 INV-12's export RSS budget, not the cache size); `features;fast` not being a real label (it is — `CMakeLists.txt:1001`). **Found during packet construction, before a lane was spent:** nothing. **Found by 4b's sweep, not by a lane:** the `**Layman:**` line still promised a preview that writes nothing, contradicting the § 2.3.1 just added, and two sub-subsections used `###` where siblings use `####`. **Executed rather than read** (4a step 2): the `export_slug` `CHECK` against real SQLite (`Ants_Terminal` fails both clauses, `ants-terminal` passes both, empty fails — so INV-6's fixture is valid), and `QDateTime::currentDateTimeUtc().toString(Qt::ISODate)` compiled against Qt 6, returning `2026-08-06T20:22:28Z`, which `isIsoZStamp()`'s regex accepts. Doc grew 318 → 575 lines; watch it, and split at § 2's seams if loop 2 is still finding structural defects. |
