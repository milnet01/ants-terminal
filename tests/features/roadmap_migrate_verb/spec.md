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
- **INV-10** — `notes[]` honours both bounds: 200 entries and 2048 characters
  of `detail` each.

## Out of scope

The handler's own two refusals — `caller_cwd_required` (the dispatcher's,
before the handler) and `no_project` (step 0a's) — are covered by inspection.
Both refuse before `run()` is entered, so neither can touch a store: the
property holds by construction rather than by fixture.
