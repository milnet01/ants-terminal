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

## CLAUDE.md before the 2026-09-26 trim

The user asked for history out of `CLAUDE.md` and for it to shrink. The rules
were kept; rationale, dated rulings, ANTS provenance and measurements moved
here. This is the previous file, verbatim, headings demoted two levels.

### Ants Terminal

Qt6/C++20 terminal emulator. Optional Lua 5.4 plugins. `libutil` for PTY.
CMake build.

#### Module map (src/)

Per-subsystem reference is [`docs/subsystems.md`](docs/subsystems.md).
Query it on demand with the `subsystem` MCP tool: `op=map` for the full
`{name, summary}` list, `op=files` / `op=recent_changes` per lane. Keep it
in sync with the code as you would any spec.

**Review lanes come from `.indie-review/partition.json`, not from
`docs/subsystems.md`** (ANTS-4793). `indie_review_partition` prefers that
override and this project commits one. To change what a review lane
covers, edit the override.

#### Data flow

`PTY → VtParser → TerminalGrid → TerminalWidget`
Reverse (DA/CPR/DSR): `TerminalGrid → ResponseCallback → PTY`

#### Build & test

```bash
cmake -G Ninja -B build && cmake --build build && ctest --test-dir build --output-on-failure -LE 'perf|e2e'
```

Tests build by default (`ANTS_TESTS=ON`); pass `-DANTS_TESTS=OFF` for a
main-exe-only iteration.

**Use Ninja, not Make.** The `JOB_POOLS` cap in `CMakeLists.txt`
(`compile_pool=max(2, nproc/4)`, `link_pool=1`) only applies under Ninja.

**The live binary runs from a home-drive copy, not the project tree**
(ANTS-2174). `launch.sh` copies the freshest build output to
`${XDG_DATA_HOME:-~/.local/share}/ants-terminal/bin/ants-terminal` and
execs that copy, so the running process shares no inode with any build
output: **you may rebuild `build/` freely during a live session**,
provided the running instance was launched by an ANTS-2174-or-later
`launch.sh`. **Do not pre-check `pgrep` before a build** — the user launches
only from the Plasma icon, so the live process is always the home copy
(user ruling, 2026-07-02). A legacy instance launched before ANTS-2174
would still run from `build/`'s inode; that case no longer arises here. Launching never writes into
the project tree, so `git status` stays clean.

**Token-frugal invocations** (pipe to `tail` so a 10k-line log stays out
of the assistant's context):

```bash
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -20
```

**Run the suite in parallel** (ANTS-2231) — the test presets do it:
`default` / `fast` at `-j4`, `workstation` at `-j2`; `debug` stays serial
(ASan is ~3× RAM) and `perf` stays serial (benchmarks must not contend).

```bash
ctest --preset=default             # parallel, perf + e2e excluded
ctest --test-dir build -j4 -LE 'perf|e2e'   # the same set, no preset wrapper
```

`-j4` is the cap tuned for this 32 GiB / earlyoom host — keep it ≤4 so a
heavy desktop session doesn't thrash. Narrower runs stay fastest:
`-R <regex>` (one suite), `-L features` (one label), `--target <bundle>`
to build only the bundle you touched.

##### Cheaper iteration loops (ANTS-1550 / ANTS-1552)

- `cmake --build build --target ants-terminal` — skip the ~11 test binaries.
- `-DANTS_TESTS=OFF` — drop test targets from the graph.
- `-DANTS_CCACHE=ON` (default) — ccache compiler launcher. **Keep the
  cache big enough**: `ccache -M 20G` once, or this Qt codebase fills the
  5 GiB default and self-evicts. `ccache -s` to check.
- `-DANTS_USE_MOLD=ON` (default when `mold` is on PATH; ANTS-2233) — links
  with mold instead of GNU ld; multi-threaded *and* lower-RSS. Auto falls
  back to the default linker when mold is absent (e.g. CI).
- `-DANTS_UNITY_BUILD=ON` — opt-in, cold full builds only. It penalises
  incremental rebuilds (a one-file edit recompiles its whole unity batch),
  so it is not wired into the `fast` preset.
- `cmake --preset=fast` — isolated `build-fast/` dir + `ANTS_LINK_POOL=2`
  (parallel test-bundle linking); ccache + PCH are unconditional defaults
  on every preset, not fast-only.

##### CMake presets

Each preset: `cmake --preset=X && cmake --build --preset=X && ctest --preset=X`.

| Preset | Use |
|---|---|
| `default` | Release + Ninja in `build/`; honours the JOB_POOLS cap. |
| `workstation` | Release in `build-workstation/`, hard-capped `-j3` for constrained hardware / heavy desktop sessions. |
| `debug` | Debug + ASan/UBSan in `build-asan/`, sanitizer env wired into `ctest --preset=debug`. |
| `fast` | Release in `build-fast/` with `ANTS_LINK_POOL=2` (parallel test-bundle linking) for hot iteration loops; isolated dir keeps `build/` warm. |

##### Backstop: `tools/safe-build.sh`

Wraps `cmake --build` in a systemd-user scope (`MemoryMax=24G` /
`MemorySwapMax=8G`) so an over-parallelism regression kills the *build*,
not the session. Reach for it after kernel / Qt-major updates. Optional
audit deps self-disable if absent. **Cppcheck gotcha:** pass
`--library=qt` or it misparses `emit` as a type. Qt projects ONLY —
elsewhere that flag breaks the parse and the TU loses all coverage
(ANTS-4094).

##### Local CI: `tools/ci-parity.sh` + the pre-push hook

**`tools/ci-parity.sh --full` IS this project's local CI check** — it
runs every job of `.github/workflows/ci.yml`, and it EXECUTES that file
rather than copying it (ANTS-5322). `tools/ci_workflow.py` runs each host
job's own `run:` steps — `build-test` (default), `cppcheck` (`--lints`),
`build-asan` (`--asan`) — with the job's env and working directories, in
`build/` and `build-asan/`. It refuses any action, expression or `if:` it
has no local meaning for, so a change to `ci.yml` reaches the local run or
stops it. `qt62-baseline` and build-test's toolchain run in podman
(`--qt62`, `--ubuntu24`). A job `ci.yml` gains that the script does not
claim fails the run. There is no second script; anything calling itself
`local-CI.sh` would be a duplicate of this one. `--stress` adds CPU load.
To hunt a flaky test, run ctest directly:
`ctest --test-dir build --repeat until-fail:5 -R <test>`.

**What no local run can catch** is a tool the GitHub runner lacks and this
machine has — the runner's packages are declared in `ci.yml`, and here they
come from the host. The repair for that class is a *static* check that the
recipes agree with the source (`tests/features/ci_workflow_deps`). That test
reads every carrier that runs the suite, not just `ci.yml` (ANTS-4717): the
workflow, the RPM spec, the Arch PKGBUILD and the Debian control. **Adding a
carrier that runs `ctest` means adding it to that test.**

**It runs before every push automatically, and it runs `ci.yml` too**
(ANTS-5322). `tools/hooks/pre-push` (wired via `core.hooksPath=tools/hooks`)
runs `ci.yml`'s `build-test` job through `tools/ci_workflow.py` — build,
the whole suite, the lints, with the job's env — and its `build-asan` job
when `build-asan/` exists and is warm (ANTS-4118's cost gate). A push is
docs-only, and skips, exactly when `ci.yml`'s push `paths-ignore` says so;
the hook keeps no copy of that list. Not in the hook: the informational
`cppcheck` job, a cold container leg, and `build-test`'s serial perf step,
which skips under the hook's `ANTS_PUSH_GATE=1` and runs on GitHub
(ANTS-5375, user ruling 2026-09-26); `--full` runs every job.

**The Qt-floor half IS covered** (ANTS-4131): the hook runs
`tools/qt62-guard.sh --warm-only`, a compile guard against the Qt 6.2
floor, whenever a push carries compilable source, and skips with a message
when it does not. So adding a source file no longer obliges a manual
`--qt62`. What that guard still cannot see is anything only the full
ubuntu:22.04 container exercises (packaging, distro Qt behaviour at
runtime).

**So is build-test's toolchain**: `tools/qt62-guard.sh --job build-test`
builds in ubuntu:24.04 with its GCC 13 and mold, and the hook runs it
`--warm-only` the same way. This box's GCC 16 links targets GCC 13 + mold
cannot, so no host build sees that class. Warm it once by running it
without `--warm-only`; `ci-parity.sh --ubuntu24` (in `--full`) runs it too.

Escape hatches: `git push --no-verify`, `ANTS_PREPUSH_NO_ASAN=1`,
`ANTS_PREPUSH_NO_QT62=1`, `ANTS_PREPUSH_NO_UBUNTU24=1`. The ASan leg is cost-gated (ANTS-4118): it runs
only over a tree whose pending ninja edges are under
`ANTS_PREPUSH_ASAN_MAX_EDGES`, and a pending CMake regen is resolved
rather than skipped (ANTS-4536). A damaged deps log is reported and the
leg still runs; `build-asan/.ants-prepush-interrupted` does skip, and the
message states the marker's age (ANTS-4943). Contract and invariants:
`tests/features/prepush_asan_gate/spec.md`.

#### Test harnesses

- **`audit_rule_fixtures`** — `tests/audit_self_test.sh` matches rule
  regexes against `tests/audit_fixtures/<rule>/{bad,good}.*` (bad: N hits
  with `// @expect <rule-id>`; good: zero). Count-based, not line-based.
- **Feature-conformance** (`tests/features/*`, label `features`) — each
  subdir pairs `spec.md` (contract) with a C++ test compiled into a shared
  bundle (not a standalone — see `tests/features/README.md`). To add one:
  (1) write `spec.md` first, surface for sign-off; (2) write
  `test_<feature>.cpp`, exit 0/non-zero with enough output to diagnose;
  (3) add the source to a bundle's `SOURCES` list (do NOT `add_executable`);
  (4) verify it FAILS against pre-fix code before restoring the fix.
  **Then build THAT bundle's target and check the ctest count moved.**
  Building the wrong target succeeds silently and runs the old binary, so
  the new test neither compiles nor appears — and a green run reads as
  success. `ctest -N -R <name>` before and after is the check; the count,
  not the pass rate, is the signal. **Ask `build_target_for` which bundle
  owns the file** (ANTS-3745): it returns the target, the `cmake --build
  --target` line and the `ctest -R` filter in one call, and `found:false`
  is exactly the not-yet-wired state above. Bundles are not guessable from
  the path — `tests/features/spec_conformance/` builds into `test_claude`,
  not `test_core`. The old recipe (`grep -n <feature> CMakeLists.txt`, then
  read upward for the enclosing `ants_add_*_bundle(`) still works and is
  the fallback outside an Ants session.
- **Perf harness** (`tools/perf-report.sh`, `tests/perf/`, label `perf`,
  ANTS-5133) — runs every `bench_*` benchmark and reports each metric
  against a saved baseline (`tests/perf/baseline.tsv`), flagging moves past
  a threshold. Excluded from the `default` / `fast` presets and not run in
  CI. How-to, including how to add one:
  [`docs/qa/perf-harness.md`](docs/qa/perf-harness.md).
- **E2E harness** (`tools/e2e/`, label `e2e`, ANTS-2049) — drive a
  throwaway `--e2e` instance as a user (inject-key/click, resize-window,
  grab-image over its socket via `--remote-json`) and observe it. Opt-in:
  `ctest -L e2e` (excluded from the default presets). Two suites:
  `smoke.sh` (harness-contract guards) and `cases.sh` (ANTS-2050
  feature lanes — `terminal`/`scrollback`/`resize`/`theme`, run all or
  `cases.sh <lane>`). Full feature checklist (auto/manual/pending-hook):
  [`docs/qa/e2e/cases.md`](docs/qa/e2e/cases.md); how-to + case format:
  [`docs/qa/e2e/README.md`](docs/qa/e2e/README.md).

#### Hot reload is the design default (user standing rule, 2026-09-21)

**Every new feature states how it reaches a running terminal without a
relaunch.** Prefer a design whose behaviour can be re-read, re-registered
or re-spawned at runtime over one that can only be rebuilt in.

**Why this outranks the usual convenience argument.** This terminal hosts
the user's Claude Code sessions in its tabs, and `~Pty`'s teardown
(`src/ptyhandler.cpp`) kills every child (`SIGHUP` → `SIGTERM` →
`SIGKILL`). `SessionManager`
persists the scrollback and cwd, not the processes. So a relaunch does
not cost a few seconds — it destroys every in-flight session across every
project, and the user must stop all of them first. That is the real price
of every compiled-in change.

**The test, applied at design time and stated in the spec or the roadmap
item:** *what would a user have to restart to get this change?* Three
acceptable answers — nothing; a re-read of a file; a reconnect by the
client. "The terminal" is the answer to avoid, and where it is
unavoidable the item says so and says why.

**What this means concretely, cheapest first.**

- **Behaviour that is DATA reloads; behaviour that is CODE does not.** A
  rule pack, a schema, a description, a theme, a keymap, a template
  belongs in a file the process re-reads, not in a string literal. The
  MCP tool list is already rebuilt per `tools/list` request
  (`claudeintegration.cpp`, the handler's local `QJsonArray tools`), so
  schema and description text moved to data would go live on a client
  reconnect with no rebuild at all.
- **Keep the GUI-dependent surface small, and the check is a grep.** The
  seam is the set of `MainWindow` methods the remote-control layer
  calls; ANTS-4932 enumerates it and is the place to look before adding
  to it. The check, and the qualifier is the whole of it: a verb that
  does **not** need tab or terminal state must introduce no new call
  into `MainWindow` from `src/remotecontrol*.cpp` or
  `src/claudeintegration.cpp`. One that genuinely does need it names the
  method in its design, reuses one ANTS-4932 already enumerates where it
  can, and adds to that enumeration where it cannot — growing the seam
  deliberately and visibly rather than by accident. Everything else is
  file and sqlite work
  that is already `MainWindow`-independent and is exercised against a
  null-`MainWindow` `RemoteControl` across the test suite. A verb that
  does not need the window must not acquire a dependency on it — that is
  what keeps ANTS-4932's out-of-process split reachable.
  **`CallerCwdContract::TabSpecific` is NOT that register**, and reading
  it as one is the trap: it covers per-tab reads that route on a `tab`
  index or `caller_cwd`, enforced as a refusal gate since ANTS-1415
  Phase 3b. A verb that lists, creates, selects or retitles tabs touches
  tab state and is deliberately absent from that table — adding one to
  it switches on a refusal its siblings do not carry.
- **A new module earns its reload story before it earns its code.** If
  the honest answer is "only a relaunch", say so in the design and let
  the user weigh it, rather than discovering it after the fact.

**What this rule does NOT require, so it does not fire on everything.**
It does not demand a plugin system, does not forbid compiled code, and
does not apply to a bug fix in existing behaviour, a test, a document or
a version bump. It governs NEW features and NEW modules at design time.
Retrofitting an existing subsystem is a roadmap item to be weighed, never
an obligation this rule imposes.

**The standing exception, stated honestly.** Compiled C++ cannot hot
reload in-process, and this project is compiled C++. So the rule is about
where BEHAVIOUR lives and how the process is DIVIDED, not about pretending
the language is something else. ANTS-4932 is the structural instance:
`ants-mcpd` serves the project-scoped MCP verbs from a process the client
starts, so a verb change needs a rebuild and an MCP reconnect, never a
terminal relaunch (`docs/specs/ANTS-4932-standalone-mcp-server.md`).
`config.json`, `audit_rules.json` and the Lua sandbox are the other cases
that work this way and are the models to copy.

#### Conventions

- Signals/slots for cross-component comms.
- Config at `~/.config/ants-terminal/config.json`, mode 0600.
- **The roadmap store is MACHINE-GLOBAL, not per-project**
  (`~/.local/share/ants-terminal/roadmap.sqlite`, mode 0600). Anything
  registered in it outlives the session that registered it and is summed by
  every `scope:"all"` surface. Four things to know before you touch it.
  **`roadmap_migrate` refuses a root under the system temp dir**
  (`transient_root`, ANTS-4600, `RoadmapMigrateVerb::isTransientRoot()`).
  That guard is in the HANDLER, not in `run()`: `run()` takes an arbitrary
  `storePath` and ANTS-3855's fixtures legitimately migrate temp roots into
  temp stores, so a guard inside it reddens the suite. **The schema declares
  `REFERENCES` but no `ON DELETE CASCADE`**, so deleting a project means
  deleting element → history → feedback_ref → relationship → citation →
  message → item → section → id_prefix → project by hand, in that order,
  with `PRAGMA foreign_keys = ON`. Two of those ten clear BOTH ends —
  `relationship` and, since ANTS-4622, `message` — because each names two
  projects. **Back it up with sqlite3 `.backup`, never `cp`** — the store
  runs in WAL and a live Ants holds a connection. **A `kSchemaVersion` bump
  is a ONE-WAY DOOR across every project on the machine** (ANTS-4462):
  `RoadmapStore::open()` refuses outright when the store's `user_version`
  exceeds the build's, which makes every roadmap verb refuse, so the first
  binary to upgrade locks every older build out of every project. Before
  reaching for a rung, ask whether the value can be DERIVED instead.
- Per-project layout is optionally declared in a repo-committed
  `<root>/.ants/project.json` (ANTS-2160, `src/projectsettings.cpp`):
  `source_roots`/`test_roots` (codebase_index), `docs_dir`,
  `roadmap`, `changelog`, `specs_dir` — each consumer falls back to its
  heuristic when a key is absent. Distinct from the global config above:
  per-project, world-readable, no secrets (NOT mode 0600). The
  `project_settings` verb (`detect`/`init`/`set`) creates and updates it.
- Scrollback default 50k, max 1M.
- Theme colors set on `TerminalGrid`; ANSI palette (16+216+24) lives there.
- QTextLayout for ligature shaping.

#### MCP tool authoring

**Discovering the full toolkit.** The SessionStart hook lists ~12
high-frequency verbs; for the full set (grouped by category, each with a
one-line *when to use*, and the live `tool_count`), call
`tool_info {catalog:true}` (ANTS-1985) once, then `ToolSearch`
`select:mcp__ants__<name>` to load a verb's schema before calling it. This
pointer is the always-loaded fallback for when the hook prelude is stale
or disabled.

**Session bootstrap refreshes the codebase map.** `session_orient` (the
documented first call) embeds and eagerly refreshes `codebase_index`, so
the codebase map is rebuilt at session start. Query it via
`codebase_index` / `find_definition` / `find_sources` / `workspace_search`
rather than `grep` (cheaper, and it's the index the first call just
refreshed).

When adding or modifying an MCP tool, follow
[`docs/standards/mcp-tools.md`](docs/standards/mcp-tools.md) (the umbrella
checklist; its *Load-bearing contracts* quick-reference lists each
contract — response-wrap, caller_cwd, CallerCwdContract, path validation,
ETag-304, `fields=`, refusal codes, state routing — with its ANTS-spec).
Per-verb behavioural reference lives in
[`docs/standards/mcp-behavioural-notes.md`](docs/standards/mcp-behavioural-notes.md).
Read both on demand; the live verb catalogue is `tool_info
{catalog:true}`.

**MCP config keys — catalogued in
[`docs/standards/mcp-config-keys.md`](docs/standards/mcp-config-keys.md)**
(ANTS-3429). The master gate is `claude.mcp_enabled` (ANTS-1901, Settings →
General; when false the socket isn't bound at launch, the auto-switcher
stands down, and every verb refuses `mcp_disabled`). The doc also covers
the autonomous model-switcher keys (ANTS-1735), result-offload keys
(ANTS-2094), the per-call `encoding:"tabular"` columnar arg (ANTS-2090),
and `project_query` keys (ANTS-2093) — read on demand.

#### Cross-session MCP feedback

Other CC sessions (Vestige, MAME Curator, Album Builder, RetroArch,
RetroDB, and others) write MCP observations to `*_Ants_MCP_Feedback.md`
files under `/mnt/Games/Scripts/Linux/`. Format spec:
[`docs/standards/mcp-feedback-files.md`](docs/standards/mcp-feedback-files.md).
The format is v2, an inline-ID model: the maintainer fills each finding's
`**Proposed ID:**` slot in place and status is derived live from the
ROADMAP, so v2 writes no tracking tables. Every corpus file is migrated.

At session start, `session_orient` surfaces a `feedback_pending` block
(ANTS-1964) — a per-file count of un-triaged contributor addenda across the
shared-root files, so you see which need triage without one
`feedback_query` per file. Only files with pending input list.

Reviewing feedback efficiently (don't re-read the whole file):

- **`feedback_query`** (ANTS-1961) — pass the feedback file's `path`;
  returns the un-triaged tail + already-mapped `ANTS-NNNN` IDs; saves ~60k
  tokens vs. a full read. Read-only, ETag-aware.
- **`feedback_log`** (ANTS-1962) — write side (`path` basename must end in
  `_Ants_MCP_Feedback.md`). Contributor: `op:"append_finding"` (append-only
  at EOF; creates the skeleton first time; stamps a blank `**Proposed ID:**`
  line). Maintainer triage: `op:"assign_id"` (ANTS-3447),
  `op:"compact_resolved"` (ANTS-3443), `op:"migrate_v2"` (ANTS-3446) for a
  file that is still v1. Per-op detail: the standard's §Tooling, which also
  carries the legacy v1 ops.

Triage flow: `feedback_query` the tail → allocate IDs via `roadmap_log
op:append` → `feedback_log op:"assign_id"` to fill each finding's
`**Proposed ID:**` slot (or an `n/a — <reason>` closure) → once an ID ships,
`op:"compact_resolved"` collapses the write-up. Filling the slot removes that
finding from the next `feedback_query` delta (no watermark to advance).

#### Project standards

**The shared standards are owned globally, at `~/.claude/standards/`.**
`coding.md`, `documentation.md`, `testing.md` and `commits.md` in
`docs/standards/` are **deltas**: each opens with the Qt/C++/Ants-specific
rules that cannot live in a language-agnostic standard, then mirrors its
owner verbatim below a divider (`security.md` is the mirror alone, no
delta). Read the delta half for what this project adds.

**Never edit a mirrored half.** A correction goes upstream, then
`tools/check-standard-mirrors.sh --write` re-copies it down;
`tools/hooks/pre-commit` refuses a commit whose mirror has drifted
(ANTS-4133).

**Check the owner is COMMITTED before running `--write`.** `~/.claude` is a
live repo another session may be editing, and the gate compares against the
owner's working tree, not its HEAD — so a review pass in flight upstream
shows up here as drift on a file you never touched, and `--write` copies a
half-written document into this public repo. `git -C ~/.claude status
--porcelain` first: if the owner is dirty, the honest move is to leave that
mirror alone and commit with `ANTS_PRECOMMIT_NO_MIRRORS=1`, saying so in the
message. Where the owner is clean, `--write` is right and the drift is real.

**Read the drift diff's direction right: `<` is the OWNER, `>` is the project
mirror.** The header says "docs/standards/X.md: DRIFTED from
~/.claude/standards/X.md", which reads as if the project file is the first
operand and therefore the `<` side. It is not. Get this backwards and the
mirror looks *ahead* of its owner — a state that should be impossible — and
the tempting repair is to edit the owner, i.e. push a mirror's text upstream,
which is the one direction the whole arrangement forbids. If in doubt, open
the owner and find the `<` text in it rather than reasoning from the header.

**Two files are NOT deltas, deliberately:**

- [`roadmap-format.md`](docs/standards/roadmap-format.md) — **this project
  is UPSTREAM of the global copy** (CFG-0069). Where the two disagree, this
  one governs and the global copy is corrected to match — never the
  reverse. It is a full standard and stays one.
- [`specs.md`](docs/standards/specs.md) — a full standard, not a delta; its
  § 0 records why. It owns a spec's **shape**; global `spec-format.md` § 1
  owns whether a spec is needed at all.

Project sub-specs. The list below is illustrative — **`docs/standards/` itself
is the roster**, and a file there binds you whether or not it is named below,
unless it marks itself superseded. Prefer it to
[`docs/standards/README.md`](docs/standards/README.md): a directory listing
cannot go stale where a hand-maintained table can, and
`tools/check-standards-index.sh` fails when a standard is missing from that
table (ANTS-4762). Most have no global counterpart; `dependencies.md` is the
exception and its entry says so:

- [`mcp-error-codes.md`](docs/standards/mcp-error-codes.md) (ANTS-1353) —
  canonical `code` taxonomy for refusal envelopes. **This is the
  authoritative taxonomy**; `mcp-errors.md` beside it is a superseded draft.
- [`mcp-caches.md`](docs/standards/mcp-caches.md) (ANTS-1439) — cache
  keying / relocation contract (never *shadow*).
- [`mcp-tools.md`](docs/standards/mcp-tools.md) — umbrella MCP-tool
  authoring checklist.
- [`dialogs.md`](docs/standards/dialogs.md) — every `QDialog` conforms to
  the theme (`DialogChrome`), is resizable, persists size, and re-centers
  on open (D1–D4).
- [`audit-false-positives.md`](docs/standards/audit-false-positives.md)
  (ANTS-1457) — `.ants_review_falsepos.jsonl` ledger contract for the
  AI-reviewer skills.
- [`status-bar.md`](docs/standards/status-bar.md) — status-bar widget
  convention.
- [`test-audit-resume.md`](docs/standards/test-audit-resume.md)
  (ANTS-1580) — `partition_token` save/resume recipe via `session_memory`.
- [`dependencies.md`](docs/standards/dependencies.md) (ANTS-3427) —
  dependency-version policy: latest stable by default (features + security);
  a below-latest pin is allowed only with a **Downgrade Ledger** row naming
  the breaking version + a re-test trigger; minimum-supported floors (Qt 6.2,
  Lua 5.4, C++20) are distinct from pins. The project-side companion to
  global `standards/dependencies.md`.
- [`mcp-config-keys.md`](docs/standards/mcp-config-keys.md) (ANTS-3429) —
  Ants-MCP config-file / Settings keys.

**Do not cite a section number of a delta file from memory.** The global
files were restructured and the numbers moved. Open the file.

ADRs live at `docs/decisions/` (Nygard format); per-feature specs at
`docs/specs/`; per-phase outcomes at `docs/journal/`. `docs/plans/` is
deprecated (historical records only).

#### Versioning & release

SemVer. **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single
source of truth** — `ANTS_VERSION` propagates everywhere; never hardcode
versions in `.cpp` / `.h`. Every bump touches `CMakeLists.txt` and
`README.md` (its `Version <strong>X.Y.Z</strong>` banner) — the
`CHANGELOG.md` **version heading** is rolled by `new-rc` and dated by
`promote`, never by the bump; bullets in the still-open `[Unreleased]` are
authored **as work lands**, plus whatever the bump-time coverage run adds
(see below); use `cut-release --bump-only` (its `.claude/bump.json` covers
the packaging files). Every cycle also **re-checks README.md's prose, not
just its version banner** — a `bump.json` todo owns the criteria; update it
only when a user-visible claim has actually drifted.
`tools/check-readme-claims.sh` (ANTS-4584) derives README's checkable
numbers from the tree and the pre-push hook runs it, so adding an MCP verb
fails the push until README is updated. Completed `ROADMAP.md` items reach
the CHANGELOG as a summary the copy rule below governs.
Update `PLUGINS.md` in the same commit when the `ants.*` Lua surface
changes.

**Release candidates (ANTS-1318).** The weekly Wednesday cadence cuts a
public release + a Patron-preview RC. The `-rcN` suffix lives ONLY at the
git tag, GitHub-release title, and AppImage filename — never in
`CMakeLists.txt` / `bump.json` (INV-3 / INV-9). RC orchestration is
`packaging/cut-rc.sh` (`new-rc` / `respin` / `promote` / `status` /
`cycle` / `hotfix`), NOT `cut-release`'s own release phases. Flow:
`cut-release --bump-only` to base `X.Y.Z`, then `cut-rc.sh new-rc --push`.
`release.yml` routes RC AppImages to a separate zsync channel so stable
users can't auto-update onto an RC.

The **guarded Wednesday cadence is `cut-rc.sh cycle`** (ANTS-2164):
promote the in-flight RC, then cut the next one — each phase self-skips
when there is nothing to do and hard-refuses an empty / placeholder /
stale / drifted RC; `new-rc` auto-rolls `[Unreleased]` and `promote`
auto-date-stamps the CHANGELOG/metainfo/debian carriers. The bump
between phases is still a separate `cut-release --bump-only` (the script
never edits version files). For an urgent bug in the already-published
release, use **`cut-rc.sh hotfix <fix-sha>…`** (ANTS-2165): two phases
around a bump that ship `vN` + the cherry-picked fix as the next public
patch and roll the in-flight RC up one number.

**`new-rc` builds and tests before it tags — run it backgrounded, never
under a short command timeout.** The preceding bump invalidates most of the
graph, so the gate is a near-full rebuild. A rehearsal killed at a
2-minute timeout is a SIGTERM'd ninja — the corruption case; check
`ninja -C build -n` and `-t recompact` before doing anything else if it
happens. `--skip-build` skips the gate, not the tagging. **Never leave the
bump to author or date a CHANGELOG _version_ section** — `.claude/bump.json`
todo 2 explains why; `new-rc` owns that roll. Bullets under the open
`[Unreleased]` are a different thing and are authored as work lands, the
bump-time coverage run included.

**Check the roadmap store for shipped-but-unrecorded work at bump time**
(ANTS-4714): `bash tools/check-shipped-coverage.sh` lists every item the
store says shipped since the last public tag that no CHANGELOG bullet
cites. It is the CONVERSE of the release skill's own gate, which only
checks that ids the CHANGELOG *claims* are really shipped — that direction
cannot see work that shipped and was never written down. Run it BEFORE
`new-rc` rolls `[Unreleased]`, because afterwards a missing entry has to go
into a closed section. It reads `roadmap.sqlite` directly because the gate
must work headless (ANTS-4734). It also reports how many shipped items
carry no date and were invisible to it; `roadmap_log op:"backfill_dates"`
fills those from git history.

**A CHANGELOG entry states what SHIPPED, never what was wrong — do not copy
a roadmap headline that states a PROBLEM into the bullet** (ANTS-4759). A
defect item's headline states one, so copying it puts the bug in the release
notes where the fix belongs. `releases.md` § 2 makes the changelog section
the description of what shipped, and that is the line this breaches. It is a
REFLEX, not a one-off, and `changelog_log op:"add_from_roadmap"` produces one
by design — **as does `op:"add_batch"` for an entry given only an id**, which
auto-detects to that same path, so the entry's shape decides rather than an
op you chose. Prefer `op:"add"` with an authored summary wherever the
headline states a problem — which is not only the `fix` kinds. Keep
`add_from_roadmap` for an item whose headline already reads as a delivered
change. `tools/check-shipped-coverage.sh` reports the copies alongside the
uncovered items and **exits non-zero on either** — it is `cut-rc.sh` that
chooses not to block, so do not wire the bump-time run into a `set -e` chain
expecting it to pass. It compares **byte-identically**, which cuts both ways:
a reworded headline that still states the problem is not caught, and a copy
this rule PERMITS is flagged anyway. It reports byte-identity, not the rule —
so review each hit rather than rewording it on sight.

#### Key design decisions (non-obvious)

- Custom VT100 parser, no pyte/libvterm. Qt6 is the only runtime dep.
- Delayed-wrap (xterm-style) for correct line wrapping.
- Alt-screen 1049 supported (vim/htop).
- Combining chars in per-line side table — zero overhead when absent.
- Image paste auto-saves and inserts the filepath (Claude Code workflow).
- Lua sandbox strips dangerous globals + instruction-count timeout.
- Session persistence via `QDataStream` + `qCompress`.
- `opacity` config drives per-pixel terminal-area fillRect alpha only;
  chrome paints opaque (`WA_StyledBackground`). No `setWindowOpacity()`
  path.
- Audit rule pack is JSON not YAML (`QJsonDocument` built-in). Hardcoded
  checks stay in C++; `audit_rules.json` only appends/overrides.
- Audit uses `clazy-standalone` (Qt-aware AST), not embedded libclang.
- `.audit_suppress` is JSONL v2 (`{key, rule, reason, timestamp}`).
- Audit calibration reads **existing** project configs rather than adding
  new suppression files; `.audit_allowlist.json` is only for custom grep
  rules with no upstream config.
- Audit test harness is shell-based against fixture dirs — no C++ unit
  framework, no link-time coupling to `auditdialog`.
- Confidence score (0-100): floor +10, `severity×15`, +20 cross-tool
  corroboration (★ tag + SARIF property), +10 external AST tool, −5 short
  grep finding, −20 test path. AI-triage caps: FALSE_POSITIVE ≤ 30,
  TRUE_POSITIVE ≥ 80.
- SARIF exports include `contextRegion` (±3 lines) + `properties.blame`.
  Generated files (`moc_*`, `ui_*`, `qrc_*`, `*.pb.cc/.h`, `/generated/`,
  `_generated.*`) auto-skipped.
- Roadmap-query IPC caches parsed bullets with mtime + 100 ms TTL
  (ANTS-1117).

---

**Why a rule here says what it says:**
[`docs/history/claude-md.md`](docs/history/claude-md.md). Pedigree,
superseded wording, dated corrections and the arguments that settled a
rule live there, not in this file. Nothing was deleted when it moved.
