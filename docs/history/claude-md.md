# CLAUDE.md — pedigree

Why the rules in the project preamble say what they say. Nothing here is
an instruction. A session follows `CLAUDE.md`; it reads this file only to
find out how a rule came to be worded the way it is.

Moved here 2026-09-21 (CFG-0492): `CLAUDE.md` is read on every turn of
every session, so a paragraph of history there is the most expensive prose
in the repository. Every passage below was in `CLAUDE.md` before that date
and is reproduced as it stood.

Follows the pattern the global config already uses — pedigree moved out of
a rule lives in `docs/history/claude-md.md`, under `documentation.md`
§ 2.8 item 4.

---

## Module map

The per-subsystem reference was moved to `docs/subsystems.md` by ANTS-1292
so the ~130-line lane catalogue is not reloaded into every Claude session
preamble.

Review lanes come from `.indie-review/partition.json` rather than
`docs/subsystems.md` (ANTS-4793). `indie_review_partition` prefers that
override, this project commits one, and it is the only one of the two that
covers every tracked `src/` file. The two are different partitions at
different granularities and that is deliberate — `docs/subsystems.md`
§ header carries the split.

## The live binary and the build tree

**ANTS-2174, an ANTS-2025 follow-up.** The Plasma icon → `launch.sh`
copies the freshest of `build/` and `build-fast/` to
`${XDG_DATA_HOME:-~/.local/share}/ants-terminal/bin/ants-terminal` (atomic
temp+rename, only when newer) and execs *that* copy. So the running
process shares no inode with any build output: an in-place relink of
`build/` while Ants is open can no longer corrupt the live code pages —
the ANTS-2025 SIGSEGV class is gone.

Verifying which binary is live, where that is ever in doubt: `pgrep -af
ants-terminal`; the path must be the home copy
(`…/.local/share/ants-terminal/bin/ants-terminal`), not
`…/build/ants-terminal`.

`build-fast/` is no longer required for safety once everyone is on the
home copy; it stays useful as an isolated/parallel build tree (the `fast`
preset).

Launching never writes into the project tree (no `build/` promote step),
so `git status` stays clean. The binary is location-independent: its only
path-relative load is the app-icon fallback
(`applicationDirPath()/../assets`), which never fires because the icon is
installed in the hicolor theme; the MCP project root is resolved per-call
from `caller_cwd` / the focused tab's cwd, not from `applicationDirPath`
(the note lives beside the `caller_cwd` resolution in
`src/remotecontrol_workspace.cpp` — applicationDirPath would be the build
dir, which is wrong); all user state lives under XDG paths.

## Why Ninja, not Make

The `JOB_POOLS` cap in `CMakeLists.txt` (`compile_pool=max(2, nproc/4)`,
`link_pool=1`) only applies under Ninja. Make ignores it, and the
resulting cc1plus over-parallelism earlyoom-reaped binaries in 0.7.x.

## Parallel test runs (ANTS-2231)

ctest is serial by default (~78 s full suite); the test presets
parallelise it — `default` / `fast` at `-j4`, `workstation` at `-j2`.
`debug` stays serial (ASan is ~3× RAM) and `perf` stays serial
(benchmarks must not contend). `ctest --preset=default` measured ~19 s.
The full suite was verified green and flake-free at `-j4`.

## Iteration loops (ANTS-1550 / ANTS-1552)

ccache at the 5 GiB default fills on this Qt codebase and self-evicts
(~40% hit rate); `ccache -M 20G` once lifts the hit rate so cold-after-pull
rebuilds reuse far more objects.

`-DANTS_USE_MOLD=ON` (ANTS-2233): linking is the heaviest, highest-RSS
step (hence `link_pool=1`); mold is multi-threaded *and* lower-RSS, so it
shortens the ~30-bundle link tail without raising the OOM ceiling.

`-DANTS_UNITY_BUILD=ON` is viable end-to-end since ANTS-1553. Unity
applies only to the always-fully-linked libs (chrome / claude / dialogs /
audit_dialog — the Widgets-heavy cc1plus hogs); the subset-linked libs
(core / vt / audit / lua) stay per-TU so the test bundles' selective
`--start-group` links don't drag cross-lib externals (esp. core's
aggregated AUTOMOC). Best for cold full builds; it penalises incremental
rebuilds, so it is not wired into the `fast` preset.

## `tools/safe-build.sh`

Wraps `cmake --build` in a systemd-user scope so a future
over-parallelism regression kills the *build*, not the session. Layers
1–2 (JOB_POOLS, `workstation` preset) should make it unnecessary; reach
for it after kernel / Qt-major updates.

## Local CI (ANTS-2134 / 3410 / 3580 / 4392 / 4717)

`tools/ci-parity.sh` is a hand-maintained parallel implementation, not a
runner for `ci.yml`. It contains no workflow parser, no `act` and no YAML
read; it re-states the jobs in shell. So the two can drift, and the drift
it cannot catch by construction is anything *declared* in `ci.yml` that
the script never knew to assume — a runner package, an env var, an action
version.

**ANTS-4391 is what that costs**: `ripgrep` was installed by no job, CI
was red for five commits, and no local run could see it because rg exists
on the dev box. The repair for that class is a *static* check that the
recipes agree with the source (`tests/features/ci_workflow_deps`), never a
parallel implementation trying harder.

Guarding one carrier turns a class defect into a queue of surfaces, each
found by a build — the RPM was the second (ANTS-4717), which is why the
check reads every carrier that runs the suite rather than `ci.yml` alone.

Driving the real workflow with `act` was considered and rejected — it
pulls container images and is slow enough that nobody would run it before
a push, and a gate nobody runs catches nothing.

CI is red where local is green when the runner's environment differs:
`C.UTF-8` POSIX collation (ANTS-2120) and a loaded 4-vCPU host that
exposes timing races (ANTS-2130).

### Why the Qt 6.2 compile guard exists (ANTS-4131)

A push that ADDS a TU once cost three consecutive red CI runs —
`spec_conformance` shipped using `QRegularExpressionMatch::hasCaptured()`,
which is Qt 6.3+ against this project's Qt 6.2 floor (`dependencies.md`
§ 4). It compiled here, passed 3393/3393, and passed the hook, because at
that time only `qt62-baseline` could see a floor violation and the hook did
not run it. A new TU is still the change most likely to reach for a Qt API
newer than the floor; it is now caught before the push rather than by CI.

### The pre-push ASan leg (ANTS-4118 / 4536 / 4943)

Cost-gated: it runs only over a tree whose pending ninja edges are under
`ANTS_PREPUSH_ASAN_MAX_EDGES`, skipping with a loud message on a cold tree
— so a caller's command timeout can no longer SIGTERM a push mid-ninja.

A pending CMake regen is resolved, not skipped (ANTS-4536): the regen
hides every real edge behind it, so the hook runs it (a CMake re-run, not
a build) and gates on what it reveals. Skipping there stood the leg down
on every push touching `CMakeLists.txt`.

Two other conditions are not the same: a damaged deps log is reported and
the leg still runs (its warning survives a `--clean-first` rebuild, so
gating on it would never clear), while `build-asan/.ants-prepush-interrupted`
does skip, because an incremental result over a killed ninja is a false
pass. That marker never expires, so the skip message states its age
(ANTS-4943).

## The perf harness (ANTS-5133)

Excluded from the `default` / `fast` presets so an ordinary test run never
pays for it, and not run in CI (wall-clock numbers a shared runner cannot
reproduce). Benchmarks are discovered from `tests/perf/` and their numbers
read from the uniform lines `tests/perf/perf_metric.h` emits, so neither
list is written down anywhere.

## The roadmap store

**`roadmap_migrate` refuses a root under the system temp dir**
(`transient_root`, ANTS-4600): a session scratchpad once got registered,
survived `registerProject()`'s INV-8 because it still existed at migration
time, and left 33 duplicate items behind under a path deleted minutes
later.

**A `kSchemaVersion` bump is a one-way door** (ANTS-4462, 2026-08-24).
Because the store is machine-global, the first binary to upgrade it locks
every older build out of every project in it: an older AppImage, a Patron
RC, another checkout still on the previous version.

So a new column is never merely additive here, whatever
`ALTER TABLE ... DEFAULT` suggests.

ANTS-4462 wanted a `store_synced_at` column and got the same answer by
rendering and comparing, which needed no migration, cost nothing to
withdraw, and was strictly more correct (a stored stamp records what the
store *believes* it published, so an edit after that stamp is invisible to
it). One render of a 2,267-item project measured ~204 ms, which is too slow
per-query and fine for an opt-in check.

## Per-project settings (ANTS-2160 / 2161)

`<root>/.ants/project.json` is consumed by codebase_index / docs_index /
roadmap_query / changelog_log / spec_query+log / current_state /
project_layout, each falling back to its heuristic when a key is absent.
ANTS-2161 added the write side: `session_orient` emits a
`project_settings_suggestion` when a project's code isn't under `src/`
(gated on a near-empty `codebase_index`), and the `project_settings` verb
(`detect`/`init`/`set`) creates and updates the file in one call.

## MCP preamble relocations

The verb catalogue and the per-verb behavioural notes were moved out of
the session preamble by ANTS-2088; the config keys by ANTS-3429; the
subsystem map by ANTS-1292. The SessionStart hook lists ~12
high-frequency verbs; `tool_info {catalog:true}` (ANTS-1985) is the
always-available full set, and the pointer in `CLAUDE.md` is the fallback
for when the hook prelude is stale (ANTS-2038) or disabled.

`session_orient` embeds and eagerly refreshes `codebase_index`
(ANTS-1637 / ANTS-2140), so the codebase map is rebuilt at session start.

The preamble's pointer to `docs/standards/mcp-behavioural-notes.md` used
to enumerate examples of what it holds — the `get_scrollback` /
`roadmap_query` / `read_region` / `codebase_index` / `apply_edits` /
`model_switch_stats` notes. The list was illustrative and the document is
the roster.

## Cross-session MCP feedback — the v1 → v2 move

**v2 went live 2026-07-10.** The format moved to an inline-ID model — the
maintainer fills each finding's `**Proposed ID:**` slot in place
(`op:assign_id`) and status is derived live from the ROADMAP, so v2 stops
writing tracking tables. Every corpus file is migrated; their old v1
tables are retained in place pending a declutter pass.

**Legacy ops, for un-migrated files only:** `op:"append_tracking"`,
`op:"compact_shipped"` (ANTS-3421), `op:"prune_tracking"` (ANTS-3442).
The v1 → v2 conversion is `op:"migrate_v2"` (ANTS-3446, plus
`backfill_from_tracking`, ANTS-3474).

`feedback_query` is version-aware (ANTS-3448): on a `: 2` file the tail is
the findings whose `**Proposed ID:**` is still unfilled; on a v1 file it
is the contributor blocks after the last maintainer tracking table.

`session_orient`'s `feedback_pending` block (ANTS-1964) is gated on the
project shipping `docs/standards/mcp-feedback-files.md`, which is how only
the Ants maintainer project gets it.

## The mirrored standards

The mirrors exist because this repo is public and an outside reader cannot
open a path in a private home directory — not as a licence to keep a
second copy of a rule. That was the pre-2026-08-12 arrangement, where
`/start-app` copied the set in with an instruction to keep them verbatim
and nothing checked it: they drifted for three months and three ended up
instructing behaviour the owner forbids. `tools/hooks/pre-commit` now
refuses a commit whose mirror has drifted (ANTS-4133).

**The dirty-owner case was hit 2026-08-14** on `security.md`, while
`documentation.md`, committed upstream the same hour, needed the ordinary
re-copy — two mirrors, two different correct answers, minutes apart.

**The diff direction was confirmed 2026-08-14** by opening the owner and
finding the `<` text in it, after exactly the misreading the rule warns
about.

`roadmap-format.md` is upstream of the global copy by user decision
2026-08-12 (CFG-0069), because the parser, the store and the migration all
live here.

`tools/check-standards-index.sh` (ANTS-4762) was added because the
hand-maintained table in `docs/standards/README.md` had omitted files no
other document named either, so a session could breach a standard it had
no route to.

The global standards were restructured on 2026-08-12 and the section
numbers moved: the symbol-not-line-numbers rule is global
`documentation.md` § 2.3 (it was § 1.7 in the old project copy), and
global `coding.md` acquired its own § 1.6 / § 1.7 on entirely different
subjects.

`docs/standards/mcp-errors.md` is a superseded (2026-05-12) draft;
`mcp-error-codes.md` is the authoritative taxonomy.

## Versioning and release

`cut-release` replaced both `/bump` and `/release`, which were deleted
2026-08-13 — the old names are gone, not aliased, so a session invoking
them gets nothing.

`tools/check-readme-claims.sh` (ANTS-4584) derives README's checkable
numbers from the tree, so the tool count is no longer the unverified one.
That script's header still quotes the old wording of the README rule as
its reason for existing.

The store stamps `shipped` only from 2026-08-20 forward, so
`tools/check-shipped-coverage.sh` (ANTS-4714) also reports how many
shipped items carry no date and were invisible to it.

It reads `roadmap.sqlite` directly, and keeps doing so: `roadmap_query`
can now return the ids (`shipped_since` on the list path, ANTS-4715), but
only to a session talking to a running instance — a shell script reaches a
verb through `--remote-json`, which needs one. That would trade a
dependency on `sqlite3` for a dependency on the GUI app being up, and the
gate must work headless (ANTS-4734).

### The changelog copy rule (ANTS-4759)

Before it was fixed, `### Fixed` announced "mutation_probe keeps mutating
the source after the transport has timed out" — a roadmap headline stating
the problem, copied into the release notes where the fix belongs.

It is a reflex, not a one-off: **14 of 56 `[Unreleased]` bullets were
verbatim copies on 2026-08-31**, written across seven separate commits.
`changelog_log op:"add_from_roadmap"` produces one by design (ANTS-1868
fixed only the line-wrapping half), and so does `op:"add_batch"` for an
entry given only an id, which auto-detects to that same path.

Six of the 14 were `enhancement`, filed as the gap they closed — so the
rule is not confined to the `fix` kinds.

### Release candidates (ANTS-1318 / 2164 / 2165)

`new-rc` builds and tests before it tags. The preceding bump edits
`CMakeLists.txt`, which regenerates `build_info` and invalidates most of
the graph, so the gate is a near-full rebuild (630 pending steps on
0.7.105). A rehearsal killed at a 2-minute timeout is a SIGTERM'd ninja —
the corruption case. It survived (`ninja -C build -n` exit 0, `-t recompact`
clean), but that is what to check if it happens again.

## Design decisions

`background_alpha` was removed as redundant in 0.7.18; `opacity` drives
per-pixel terminal-area fillRect alpha only, and chrome paints opaque
(`WA_StyledBackground`). There is no `setWindowOpacity()` path.

`.audit_suppress` v1 plain-key lines load and convert on first write; the
format is JSONL v2 (`{key, rule, reason, timestamp}`).
