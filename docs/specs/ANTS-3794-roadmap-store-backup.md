# ANTS-3794 — publish the roadmap export weekly and report a backup that stops

**Status:** accepted (2026-09-19), review-contract loops 1 + 2 folded (cap reached).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3794 (ANTS-3758 split, seam 3c; user decisions 2026-09-19).
**Pairs with:** [ANTS-3761](ANTS-3761-roadmap-export-format.md) — the export's bytes; this spec runs and publishes it.
**Blocker for:** ANTS-5244 (restore from the export).

**Layman:** Every week the roadmap database is written out as text, one file per project, and pushed to a private repo; if that ever stops working, the next Claude session says so.

## 1. Problem

[`roadmap-data-model.md`](../standards/roadmap-data-model.md) § 9 leaves
four decisions to a spec: the publish cadence for the export, what a push
conflict does, how a silent backup failure is detected, and concurrency
across projects sharing one store. None is decided, so:

1. **Nothing runs the export.** `RoadmapExport::exportProject()` ships
   (ANTS-3761), but its only callers are tests. No `roadmap-export/`
   directory exists in `claude-config`.
2. **The store is the only complete copy.** Each project's `ROADMAP.md`
   omits `internal` items, history and provenance
   (`roadmap-data-model.md` § 1). Losing the store loses those.
3. **A backup that stops is invisible.** The stop-gap already shipped —
   `tools/roadmap-store-backup.sh` on a weekly user timer — notifies only
   when it runs and fails. A timer that never fires raises nothing.

The whole-store snapshot cannot go to `claude-config`. The store is one
file already past GitHub's 50 MB warning size (`stat -c %s` on
`RoadmapStore::defaultPath()`, 2026-09-19), GitHub refuses files over
100 MB, and a binary file changes whole on every write.

**User decisions (2026-09-19):** back up only to a private destination,
preferably `claude-config` (`milnet01/claude-config`, private). Weekly is
enough. Ship the local snapshot first, then this.

## 2. Surface

### 2.1 The headless entry point

```
ants-terminal --export-roadmaps <dir>
```

`main()` scans `argv` for `--export-roadmaps` **before** it constructs
`QApplication`. When present it constructs `QCoreApplication` instead
and returns
`RoadmapExport::runExportCommand(RoadmapStore::defaultPath(), dir, out)`,
with `out` a `QTextStream` on stdout. No window-system connection is
made, so a systemd user service can run it with no display.

New in `src/roadmapexport.{h,cpp}`, so the test bundles can call it
without linking `main.cpp`:

```cpp
namespace RoadmapExport {
int runExportCommand(const QString &storePath, const QString &dir,
                     QTextStream &out);
}
```

Its steps, in order:

1. If `storePath` does not exist, print an error and return **2**. Never
   create a store here.
2. Open `RoadmapStore(storePath, kDefaultHistoryCapBytes,
   Access::Bulk)`. A failed `open()` returns **2** — this includes a
   store whose schema is newer than the binary.
3. Call `RoadmapExport::exportAllProjects()` (§ 2.2). Print one line per
   written, failed and removed file.
4. Return **0** when nothing failed. Return **1** when any project failed
   or the result carries an `error`.

### 2.2 Exporting every project

New in `src/roadmapexport.{h,cpp}`:

```cpp
namespace RoadmapExport {
struct ExportAllResult {
    QStringList written;   // export_slugs whose file was committed
    QStringList failed;    // "slug: reason"
    QStringList removed;   // file names deleted as orphans
    QString     error;     // set when the run stopped before any export
};
ExportAllResult exportAllProjects(RoadmapStore &store, const QString &dir);
}
```

New in `src/roadmapstore.{h,cpp}`: `QVector<ProjectRow>
RoadmapStore::listProjects(QString *error = nullptr) const`, every
project row ordered by `export_slug`.

`exportAllProjects()`:

- Creates `dir` if absent.
- Sets `error` and exports nothing when `listProjects()` fails or returns
  no rows. An empty list is treated as a fault, not as "delete every
  file".
- Calls `exportProject(store, slug, dir + "/" + slug + ".jsonl")` for each
  project. A failure is recorded in `failed` and the next project runs.
- **Only when `failed` is empty**, deletes each `*.jsonl` directly in
  `dir` whose base name is not a listed `export_slug`. It touches no
  other file and no subdirectory. Git history keeps the deleted file.

Each project's export is already its own deferred read transaction and
takes `ConfigWriteLock` on its file (ANTS-3761 § 2.6). A running Ants
writing to the store meanwhile is safe under WAL.

### 2.3 Publishing to `claude-config`

New script `tools/roadmap-export-publish.sh <repo>`, where `<repo>` is a
clone of `claude-config` (on this machine, `~/.claude`). It exports into
`<repo>/roadmap-export/`, the path ANTS-3761 § 2.1 fixes.

Steps, stopping at the first failure:

1. Take `flock -n` on `<state>/roadmap-backup-export.lock` (§ 2.4). If it
   is held, print that another run is in progress and exit **3**. Write
   no record and touch no repository.
2. Check `<repo>` is a git work tree with a current branch that has an
   upstream, and no merge, rebase or cherry-pick in progress.
3. `git fetch` the upstream's remote. **If the upstream has any commit
   `HEAD` lacks, stop.** The error lists those commits and says whether
   any touch `roadmap-export/`. The script never merges, rebases or
   pushes in that state. This is the divergence § 9 requires to surface.
4. Run the binary (`$ANTS_ROADMAP_EXPORT_BIN`, default
   `${XDG_DATA_HOME:-$HOME/.local/share}/ants-terminal/bin/ants-terminal`)
   with `--export-roadmaps <repo>/roadmap-export`. A non-zero exit is a
   failure.
5. `git add -A -- 'roadmap-export/*.jsonl'`, then
   `git commit -m "chore: weekly roadmap export (<date>)" -- 'roadmap-export/*.jsonl'`.
   `claude-config`'s `commit-msg` hook requires an id or a
   `chore:`/`docs:`/`fix:` prefix there, and rejects a bare component
   prefix. The pathspec limits the commit to the export files. `ConfigWriteLock`
   leaves a `<file>.lock` beside each export, and those persist between
   runs (`src/configbackup.h`), so they are never staged. Other staged or
   unstaged changes in `<repo>` stay exactly as they were. Hooks run; a
   hook failure is a backup failure. When there is nothing to commit,
   skip to step 7.
6. `git push`. A rejected push is a failure. Local commits other than
   this one are pushed too; `claude-config` pushes every commit anyway
   (global `CLAUDE.md` rule 6a).
7. Record success (§ 2.4).

Every failure records the error (§ 2.4), raises `notify-send -u critical`
when it is installed, and exits non-zero. Step 1's lock-held exit 3 is
not a failure: it neither records nor notifies.

### 2.4 The backup record

Each job writes one plain-text record:

```
${XDG_STATE_HOME:-$HOME/.local/state}/ants-terminal/roadmap-backup-<job>.state
```

`<job>` is `snapshot` (`tools/roadmap-store-backup.sh`) or `export`
(`tools/roadmap-export-publish.sh`). The file holds three `key=value`
lines:

```
attempt=2026-09-21T00:36:32Z
success=2026-09-21T00:36:34Z
error=
```

- Times are UTC ISO-8601 (`date -u +%Y-%m-%dT%H:%M:%SZ`).
- `attempt` is set on every run that got past the lock.
- `success` changes only on success. `error` is emptied on success and
  set, flattened to one line, on failure.
- All three keys are always written. A value not yet set is empty, and
  `assess()` reads an empty value as absent.
- Written temp-then-`mv`, so a reader never sees half a file.

Plain `key=value` rather than JSON, so the shell writer needs no JSON
tool.

`tools/roadmap-store-backup.sh` gains the same record under
`job=snapshot`. Its behaviour is otherwise unchanged.

### 2.5 Reporting a backup that stopped

New `src/roadmapbackuphealth.{h,cpp}` in the library `session_orient`
links:

```cpp
namespace RoadmapBackupHealth {
// Empty object when healthy or when there is no store; otherwise the block.
QJsonObject assess(const QString &stateDir, const QDateTime &nowUtc,
                   bool storeExists);
constexpr qint64 kStaleAfterSecs = 8 * 24 * 60 * 60;  // weekly + one day
}
```

`assess()` returns an empty object when `storeExists` is false. Otherwise
it reads both records and classifies each job:

| State | Condition |
|---|---|
| `never_run` | record absent or unparseable |
| `failing` | `error` is non-empty |
| `stale` | `success` absent, or older than `kStaleAfterSecs` |
| healthy | none of the above |

The first matching row wins. When both jobs are healthy it returns an
empty object. Otherwise it returns
`{jobs: {<job>: {state, success, error}}, hint}`, listing only the
unhealthy jobs. `hint` names the record path and that job's timer:
`ants-roadmap-backup.timer` for `snapshot`, `ants-roadmap-export.timer`
for `export`.

`RemoteControl::cmdSessionOrient()` sets `result["roadmap_backup"]` from
`assess(<state dir>, now, QFileInfo::exists(RoadmapStore::defaultPath()))`
only when the object is non-empty — the `feedback_pending` pattern. The
state dir is `$XDG_STATE_HOME/ants-terminal`, or `~/.local/state/ants-terminal`
when that variable is unset. `QStandardPaths::StateLocation` is Qt 6.7+,
above the Qt 6.2 floor, so it is not used.

The store is machine-global, so every project's `session_orient` carries
the block. That is deliberate: whoever is working should see it.

### 2.6 Scheduling

A weekly user timer runs the publish script, beside the snapshot timer
(`ants-roadmap-backup.timer`)
already installed. Unit files are local machine configuration and are not
shipped; this is their text:

```ini
# ~/.config/systemd/user/ants-roadmap-export.service
[Unit]
Description=Publish the Ants Terminal roadmap export (ANTS-3794)

[Service]
Type=oneshot
ExecStart=/mnt/Games/Scripts/Linux/Ants_Terminal/tools/roadmap-export-publish.sh %h/.claude

# ~/.config/systemd/user/ants-roadmap-export.timer
[Unit]
Description=Weekly Ants Terminal roadmap export (ANTS-3794)

[Timer]
OnCalendar=weekly
Persistent=true
RandomizedDelaySec=1h

[Install]
WantedBy=timers.target
```

`Persistent=true` runs a missed week at the next boot. § 2.5 catches a
timer that is disabled or lost.

### 2.7 Alternatives rejected

- **Commit the whole-store snapshot to `claude-config`.** GitHub refuses
  files over 100 MB, the store is past the 50 MB warning, and each commit
  stores the whole file again. The per-project JSONL turns a week's
  changes into line diffs.
- **A separate export executable.** It would be a second binary to build,
  package and keep on the store's schema. The home copy of `ants-terminal`
  is already the binary that matches the live store.
- **Rebase or merge on divergence.** § 9 forbids auto-merge. A rebase
  over unrelated upstream commits still publishes history nobody reviewed.
  Stopping costs at most one missed week, which § 2.5 reports.
- **Detect failure by notification alone.** A notification needs the job
  to run. A disabled timer never runs, so only a reader that checks the
  record's age can see it.
- **JSON records.** The writer is shell, and JSON escaping there needs
  `jq` or `python3`. `key=value` needs neither.

## 3. Invariants

- **INV-1** — `--export-roadmaps` is handled before `QApplication` is
  constructed. Breaks if the check moves after `QApplication app(`.
  *Test:* `tests/features/roadmap_export_all` source check that the
  `--export-roadmaps` scan precedes `QApplication app(` in `src/main.cpp`;
  manual recipe § 6.2.
- **INV-2** — `exportAllProjects()` writes one `<export_slug>.jsonl` per
  listed project, byte-identical to `exportProject()` for that slug.
  Breaks if a project is skipped or written by another path. *Test:*
  `tests/features/roadmap_export_all`, two-project fixture store.
- **INV-3** — one project's failure does not stop the others; it appears
  in `failed` and `runExportCommand()` returns 1. Breaks if the loop returns on
  the first failure. *Test:* `tests/features/roadmap_export_all`, the
  first slug's destination pre-created as a directory.
- **INV-4** — with no file at `storePath`, `runExportCommand()` returns 2
  and creates no store file. Breaks if `RoadmapStore::open()` is reached.
  *Test:* `tests/features/roadmap_export_all`, a `storePath` in an empty
  temporary directory.
- **INV-5** — when `listProjects()` returns no rows, `error` is set and no
  file in `dir` is written or deleted. Breaks if an empty list is treated
  as "every file is an orphan". *Test:* `tests/features/roadmap_export_all`,
  empty store plus a pre-existing `a.jsonl`.
- **INV-6** — orphan `*.jsonl` files are deleted only when `failed` is
  empty; other files and subdirectories are never touched. Breaks if
  deletion runs after a failure or matches other names. *Test:*
  `tests/features/roadmap_export_all`, with `orphan.jsonl`, `notes.txt`
  and `sub/x.jsonl` present, run once clean and once with one failure.
- **INV-7** — the publish commit contains only `roadmap-export/*.jsonl`
  paths, and every other staged or unstaged change in the repository is
  unchanged. Breaks if the script commits without a pathspec or stages
  the whole directory. *Test:* `tests/features/roadmap_export_publish`, a
  clone with a staged and an unstaged unrelated edit, and a stub binary
  that also leaves a `.jsonl.lock` file.
- **INV-8** — when the upstream has a commit `HEAD` lacks, the script
  exits non-zero, runs no export, and creates no commit and no push.
  Breaks if it pulls, merges or rebases. *Test:*
  `tests/features/roadmap_export_publish`, a second clone pushes first.
- **INV-9** — every failure leaves `error` non-empty and `success`
  unchanged; every success, including "nothing to commit", empties `error`
  and advances `success`. Breaks if a failure path exits without writing
  the record. *Test:* `tests/features/roadmap_export_publish`, a stub
  binary exiting 1 and one exiting 0.
- **INV-10** — a second publish run while the lock is held exits 3 and
  changes neither the record nor the repository. Breaks if the lock is
  taken after the record is written. *Test:*
  `tests/features/roadmap_export_publish`, holding the lock with `flock`.
- **INV-11** — `assess()` returns an empty object when both jobs are
  healthy or `storeExists` is false, and otherwise names exactly the
  unhealthy jobs with the § 2.5 state. Breaks if a stale job is reported
  healthy or a healthy one is listed. *Test:*
  `tests/features/roadmap_backup_health`, injected `nowUtc` either side
  of `kStaleAfterSecs`, a failing record, a missing record.
- **INV-12** — `tools/roadmap-store-backup.sh` writes the `snapshot`
  record with the same rules as INV-9. Breaks if the snapshot script
  exits without writing it. *Test:*
  `tests/features/roadmap_export_publish`, which also drives the
  snapshot script against a temporary store.

## 4. RAM / build cost

- The export streams each project (ANTS-3761 INV-12's 4 MiB peak), one
  project at a time, so peak memory does not grow with the number of
  projects.
- `assess()` reads two small text files once per `session_orient`. It
  holds nothing afterwards.
- New sources join existing libraries: `roadmapbackuphealth.cpp` and the
  export changes join the libraries their neighbours are in. No new
  external library. Two new test directories join existing bundles; the
  publish test is a shell test registered like `prepush_asan_gate`.

## 5. Out of scope

- **Restoring from the export** — deferred, tracked by ANTS-5244.
  `RoadmapExport::rebuildProject()` exists; nothing outside the tests
  reaches it.
- **An export older than a schema bump** — tracked by ANTS-3860.
- **Syncing stores across machines** — a permanent exclusion. The store
  is per-machine; this spec only makes a second store's pushes visible
  (INV-8).
- **Retrying a failed run** — a permanent exclusion. The next weekly run
  retries, and § 2.5 reports the gap in between.
- **Shipping the unit files** — a permanent exclusion. They name
  machine-specific paths; § 2.6 carries their text.

## 6. Tests

### 6.1 Automated

- `tests/features/roadmap_export_all/` — INV-1, INV-2, INV-3, INV-4,
  INV-5, INV-6. C++, added to
  the bundle `build_target_for` reports for `src/roadmapexport.cpp`.
- `tests/features/roadmap_backup_health/` — INV-11. C++, same bundle.
- `tests/features/roadmap_export_publish/` — INV-7, INV-8, INV-9, INV-10,
  INV-12.
  A shell test over throwaway git repositories (a bare remote and two
  clones) and a stub binary set through `ANTS_ROADMAP_EXPORT_BIN`, so it
  needs no network and no real store. Registered with
  `add_test(... COMMAND bash ...)` like `prepush_asan_gate`, with
  `SKIP_RETURN_CODE 77`. It exits 77, naming the tool, when `git`,
  `flock` or `sqlite3` is absent, so CI and the package builds skip it
  loudly rather than fail. That skip path keeps these tools out of
  `tests/features/ci_workflow_deps`' required set.

Label `features;fast`. Each test is shown to fail against the source
before its fix, per the project convention.

### 6.2 Manual recipe (INV-1, and the real push)

1. `env -u DISPLAY -u WAYLAND_DISPLAY ants-terminal --export-roadmaps
   /tmp/x` exits 0 and writes one file per registered project.
2. `systemctl --user start ants-roadmap-export.service`, then
   `systemctl --user show -p Result ants-roadmap-export.service` reads
   `Result=success`, and `claude-config` on GitHub has the commit. This
   checks git credentials work under the user service.
3. Disable the timer, set the `export` record's `success` nine days back,
   and confirm `session_orient` carries `roadmap_backup` naming `export`
   as `stale`.

## 7. Cross-doc impact

- `claude-config`'s `.gitignore` is an allowlist. It gains these three
  lines, which track the `*.jsonl` exports and keep the `.lock` files and
  any subdirectory ignored:

  ```
  !/roadmap-export/
  /roadmap-export/*
  !/roadmap-export/*.jsonl
  ```

  That edit is made from a `~/.claude` session, in that repository.
- [`roadmap-data-model.md`](../standards/roadmap-data-model.md) § 9:
  mark the cadence, divergence, detection and concurrency items as owned
  here.
- CHANGELOG `### Added` for `--export-roadmaps` and the `roadmap_backup`
  block.
- `docs/standards/mcp-behavioural-notes.md`: the `roadmap_backup` field
  of `session_orient`.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-19 | 3, cold, identical shared packet | 1 | 0 | 1 | 2 | Verified 4, fixed 4, dismissed 1. Q1 (found building the packet): the publish step staged all of roadmap-export/, which would commit the .lock files ConfigWriteLock leaves beside each export; it now stages *.jsonl only (checked in a throwaway repo). Q4, all three lanes: INV-3 and INV-4 tested exit codes from main(), which no test bundle links; the steps now live in RoadmapExport::runExportCommand(storePath, dir, out). Q3: the claude-config allowlist line would have exposed the lock files to any git add -A; replaced by three lines that track *.jsonl only (checked). Q4: the publish shell test needs git, flock and sqlite3; it now exits 77 under SKIP_RETURN_CODE, so it stays out of ci_workflow_deps' required set. Dismissed: git's glob crossing / (nothing writes subdirectories). Four open questions resolved clean: no schema upgrade beyond the running instance, exit 3 is not a failure, library placement is local, export_slug is NOT NULL. |
| 2 | 2026-09-19 | 3, cold, identical shared packet | 1 | 1 | 2 | 0 | Verified 4, fixed 4, dismissed 2. Cap reached (2 for a spec): shipped. Q1 (found resolving two lanes' open question on hooks): the commit subject 'roadmap-export: ...' fails claude-config's commit-msg hook (exit 1), so every weekly run would fail; now 'chore: weekly roadmap export (<date>)' (exit 0). Q2: the lock-held exit 3 contradicted 'every failure records and notifies'; now stated as not a failure. Q3: all three record keys are always written, and an empty value reads as absent. Q3: the snapshot timer is named (ants-roadmap-backup.timer), and the hint names each job's timer. Dismissed: an ignored roadmap-export/ making 'nothing to commit' read as success (refuted: git add exits 128 'did not match any files'); the 4 MiB figure under Access::Bulk (true, builds nothing different). Resolved clean: the newer-schema refusal is in createSchema(); cross-project rows are ANTS-5244's. Calm cap: 1 of 4 final-loop findings anchored on text this run wrote (§2.3 step 5). Gated span 977b0b69 is the whole new spec, so all 8 verified findings fall inside it. |
