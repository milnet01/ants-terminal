# roadmap_migrate — feature contract

Design contract: [`docs/specs/ANTS-3855-roadmap-migrate-verb.md`](../../../docs/specs/ANTS-3855-roadmap-migrate-verb.md).
That document owns the surface, the refusal table and the reasoning; this file
records what the test binary asserts and how.

## What the feature is

`roadmap_migrate` is the MCP verb that loads one project's markdown roadmap into
the roadmap store. Before it, the store's read half, load half, archives,
render, export, section provenance, consumer cutover and write half had all
shipped and **nothing in production called any of them** — so no project could
be migrated.

## Routing

Every behavioural invariant drives `RoadmapMigrateVerb::run(storePath, req)`
directly, with `storePath` inside a `QTemporaryDir`. The registered handler
`RemoteControl::cmdRoadmapMigrate` is deliberately **not** driven by any test:
its remaining job is to resolve `RoadmapStore::defaultPath()`, which is the
developer's real store, and a test that touched it would corrupt live data —
which is how ANTS-3856's leaked fixture row got there. INV-2(b)'s source grep
covers that line instead.

A clause that inspects rows opens the test's own `Access::Interactive`
`RoadmapStore` at the same `storePath` **after** `run()` returns, because
`run()` owns its connection for the duration of the call and hands back only a
`QJsonObject`.

| Invariant | Route |
|---|---|
| INV-1 | source grep only |
| INV-2 | (a) `run()` against a temp store · (b) source grep |
| INV-3..INV-10 | `run()` against a temp store |

## Invariants

- **INV-1** — A non-test translation unit calls all three migration entry
  points (`RoadmapMigrate::findRoadmaps`, `RoadmapMigrate::planFrom`,
  `RoadmapMigrateLoad::load`), and only `src/roadmapmigrateverb.cpp` does. The
  scan is in-process over `ANTS_SRC_DIR` and skips lines whose first
  **non-whitespace** characters are `//` — the two surviving mentions are
  indented member comments in `src/roadmapstore.h` and `src/roadmapmigrate.h`.
- **INV-2** — `run()` migrates on an `Access::Bulk` connection it opened itself
  and cannot reach the process-owned `Access::Interactive` one. (a) `run()`
  against a fresh temp store returns `ok:true` — an `Interactive` connection is
  refused outright by `load()`'s ANTS-3765 INV-12 check, so the two outcomes
  differ. (b) None of the verb's three files names `roadmapStoreOrNull` on a
  code line, and `run()`'s declared first parameter is a
  `const QString &storePath` rather than a `RoadmapStore &`.

## Why the seam is its own translation unit

`RoadmapMigrateVerb::run()` lives in `src/roadmapmigrateverb.cpp`, apart from
the handler in `src/remotecontrol_roadmap_migrate.cpp`, and that is a
**link-time requirement**. A static archive is pulled in at object granularity,
so a seam sharing an object with `RemoteControl::cmdRoadmapMigrate` drags
`RemoteControl` → `ants::resolveCallerCwdRoot` → `MainWindow` into anything
that links it — and `test_core` links `ants_core_lib` **alone**, with no
`ants_chrome_lib`. Measured 2026-08-06: the one-TU shape failed `test_core`'s
link with ~20 undefined `MainWindow` / `ClaudeIntegration` / `AuditEngine`
symbols. A seam that cannot be linked apart from the thing it exists to be
tested apart from is not a seam.
- **INV-3** — `dry_run:true` commits no row and reports the counts the real run
  produces. `project_id` is `0` on the dry run and non-zero on the real one;
  `sources[].path` is relative on both.
- **INV-4** — No refusal **adds or changes** a row. Stated as *unchanged*, not
  *zero*, because two of the refusals presuppose an existing `project` row.
- **INV-5** — One stamp per call, in the shape `history.changed_at` CHECKs. The
  fixture is a re-run over an **edited** source: `Loader::recordHistory()` is
  reached only from the field-update path, so a first-ever migration appends no
  `history` row at all.
- **INV-6** — An invalid `export_slug` or an empty `project_name` refuses
  `bad_args` with **no store file created** at the path the test named.
- **INV-7** — A second run over an unchanged project is idempotent.
  `elements_written` is deliberately excluded: § 2.6 rebuilds element rows
  wholesale, so it is non-zero even on an unchanged re-run.
- **INV-8** — After a successful run the project resolves through ANTS-3793's
  consumer dispatch against the same store.
- **INV-9** — A store this verb creates is mode 0600. The test sets `umask(022)`
  for the duration of the call and restores it, or the leg passes vacuously on a
  machine already running `umask 077`.
- **INV-10** — `notes[]` honours its bounds: 2048 characters of `detail` each,
  and 200 DISTINCT `(code, detail, source_index)` rows after ANTS-4649 collapses
  repetition. A merged row carries `count` + up to three `sample_lines` and no
  `line`; every row carries `count`, so the counts sum to `notes_count`.
  `notes_collapsed` (rows MERGED, nothing lost) and `notes_truncated` (rows
  DROPPED, unrecoverable) are two facts and stay two fields.

## Out of scope

The handler's own two refusals — `caller_cwd_required` (the dispatcher's,
before the handler) and `no_project` (step 0a's) — are covered by inspection.
Both refuse before `run()` is entered, so neither can touch a store: the
property holds by construction rather than by fixture.

## ANTS-4617 — `op:"deregister"`, the inverse the catalogue never had

`roadmap_migrate` had no inverse, and the store is **machine-global**. Migrating
a scratch copy to test something destructive in isolation is the careful
instinct, and it left a permanent row that `roadmap_query mode:"report"
scope:"all"` sums into machine-wide figures forever. The incentive ran the wrong
way: the store penalised the safe thing to do.

Distinct from ANTS-4600's `transient_root` guard, which stops a scratchpad under
the **system temp dir** being registered at all. The root that prompted this was
not under the temp dir, so that guard does not fire and the row is legitimate at
write time.

**The guard is the design.** Deregistering a live project is data loss with no
undo — the store is primary and `ROADMAP.md` is its render, so the rows are the
only copy of the history, relationships and citations the file does not carry.
It refuses `confirm_required` while the root still exists, checked against the
**stored** root rather than the caller's argument, so keying by slug cannot skip
it. An absent root is the case the item was filed for and needs no ceremony.

The delete is one transaction across all nine tables in foreign-key order, under
`PRAGMA defer_foreign_keys` — `section.parent_id` self-references, so a single
`DELETE` over a project's sections would otherwise fail the moment it removed a
parent before its child. Deferring moves enforcement to `COMMIT` rather than
removing it, so a mistake still fails loudly and rolls back whole. Relationships
are cleared from **both** ends: a row in another project pointing into this one
would otherwise dangle, which is the one way this delete could corrupt a project
it was not aimed at.

- **`Ants4617DeregisterRemovesEveryTablesRows`** — every table is empty after.
- **`Ants4617DeregisterLeavesSiblingProjectsIntact`** — **the case that
  matters.** Two equal projects, one deregistered; the survivor keeps its
  project row and its items, and exactly half the rows in every table remain. A
  delete that reached past its own project would be far worse than the clutter
  this was written to remove.
- **`Ants4617RefusesWhileTheRootStillExists`** — `confirm_required`, and nothing
  is deleted.
- **`Ants4617AbsentRootNeedsNoConfirmAndSlugIsAKey`** — the filed case: files
  deleted, keyed by the slug, no confirm needed.
- **`Ants4617DryRunDeletesNothing`** — the count comes from a **read**, never
  from a rolled-back delete. This is the one verb whose preview is run precisely
  because the caller fears the real call, so a preview that performed the delete
  to measure it would be the opposite of reassuring. ANTS-4463's tense rule
  holds: no `deregistered` field on a preview.
- **`Ants4617UnknownProjectIsNotFound`** — not a silent success. A prune loop
  reading `ok:true` for a project it never removed would report a clean store it
  had not cleaned.

## ANTS-4621 — the schema has to declare what the handler reads

ANTS-4617 shipped `op` and `confirm` documented at length in the verb's
**description** and declared in neither the schema's `properties` nor any enum.
The schema sets `additionalProperties: false`, so a strictly validating MCP
client refuses the call before the handler is ever entered.

The permissive path is the worse one, and it is the one that was measured.
ANTS-2175 builds its `ignored_args` advisory from `inputSchema.properties`, so a
live deregister after the 2026-08-22 relaunch answered:

```
"ignored_args":["op"], "code":"confirm_required"
```

— the envelope telling the caller its argument was ignored, on the very call
that argument had just steered. A caller who believes that advisory concludes
deregister never ran.

**Why ANTS-4617's own tests could not catch it.** Every one of them drives
`RoadmapMigrateVerb::deregister()` directly. That is correct for the engine —
the seam exists so a test can link it without dragging `MainWindow` — but it
means no test crossed the dispatcher, and the schema is the dispatcher's half of
the contract. The gap is structural rather than an oversight, so the guard is
written against the **source**, which is the only place both halves are visible.

- **INV-11** — every argument `cmdRoadmapMigrate` reads via `req.value()` is
  declared in the verb's `inputSchema.properties`, `caller_cwd` excepted as a
  universal dispatch-layer arg (ANTS-2175 INV-2). Asserted by scraping the
  handler for its `req.value()` keys rather than from a hardcoded list, so an
  op added later and left undeclared fails without anyone remembering to extend
  the test.

- **INV-12** — (ANTS-4740) `op:"init"` writes a conforming ants-v1 skeleton for
  a project that has none, then falls through to the ordinary migrate so
  registration takes one code path. The skeleton parses with zero bullets and
  carries exactly one section, whose slug is what `roadmap_log`'s `section`
  argument takes. It refuses `roadmap_exists` rather than overwriting.

- **`Ants4740InitSkeletonIsParseableAndEmpty`** — INV-12's skeleton, asserted
  through `RoadmapParse::parseBullets` rather than by eye: a file that only
  looks right migrates cleanly and drops fields silently.
- **`Ants4621SchemaDeclaresDeregisterArgs`** — `op` and `confirm` are declared,
  and `op`'s enum carries `deregister`; without the enum value a client offering
  completions never surfaces the op at all.
- **`Ants4621HandlerReadsOnlyDeclaredArgs`** — INV-11 in general form. Both
  assertions fail against the pre-fix tree.
