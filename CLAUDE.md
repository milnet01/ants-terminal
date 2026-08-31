# Ants Terminal

Qt6/C++20 terminal emulator. Optional Lua 5.4 plugins. `libutil` for PTY.
CMake build.

## Module map (src/)

Per-subsystem reference moved to [`docs/subsystems.md`](docs/subsystems.md)
(ANTS-1292) so the ~130-line lane catalogue is not reloaded into every
Claude session preamble. Query it on demand with the `subsystem` MCP tool:
`op=map` for the full `{name, summary}` list, `op=files` / `op=recent_changes`
per lane. `indie_review_partition` derives one review lane per entry. Keep
`docs/subsystems.md` in sync with the code as you would any spec.

## Data flow

`PTY → VtParser → TerminalGrid → TerminalWidget`
Reverse (DA/CPR/DSR): `TerminalGrid → ResponseCallback → PTY`

## Build & test

```bash
cmake -G Ninja -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Tests build by default (`ANTS_TESTS=ON`); pass `-DANTS_TESTS=OFF` for a
main-exe-only iteration.

**Use Ninja, not Make.** The `JOB_POOLS` cap in `CMakeLists.txt`
(`compile_pool=max(2, nproc/4)`, `link_pool=1`) only applies under Ninja;
Make ignores it, and the resulting cc1plus over-parallelism earlyoom-reaped
binaries in 0.7.x.

**The live binary runs from a home-drive copy, not the project tree
(ANTS-2174, an ANTS-2025 follow-up).** The Plasma icon → `launch.sh` copies the
freshest of `build/` and `build-fast/` to
`${XDG_DATA_HOME:-~/.local/share}/ants-terminal/bin/ants-terminal` (atomic
temp+rename, only when newer) and execs *that* copy. So the running process
shares no inode with any build output: an in-place relink of `build/` while
Ants is open can no longer corrupt the live code pages — **the ANTS-2025
SIGSEGV class is gone, and you may rebuild `build/` freely during a live
session** *provided the running instance was launched by this (ANTS-2174 or
later) `launch.sh`*. Verify with `pgrep -af ants-terminal`: the path must be
the home copy (`…/.local/share/ants-terminal/bin/ants-terminal`), NOT
`…/build/ants-terminal`. A legacy instance launched before this change still
runs from `build/`'s inode and remains vulnerable until relaunched — keep
building to `build-fast/` while such a process is live. `build-fast/` is no
longer required for safety once everyone's on the home copy; it stays useful as
an isolated/parallel build tree (the `fast` preset). Launching also never
writes into the project tree (no `build/` promote step), so `git status` stays
clean. The binary is location-independent: its only path-relative load is the
app-icon fallback (`applicationDirPath()/../assets`), which never fires because
the icon is installed in the hicolor theme; the MCP project root is resolved
per-call from `caller_cwd` / the focused tab's cwd, NOT from `applicationDirPath`
(the note lives beside the `caller_cwd` resolution in
`src/remotecontrol_workspace.cpp` — applicationDirPath would be the build dir,
which is wrong); all user state lives under XDG paths.

**Token-frugal invocations** (pipe to `tail` so a 10k-line log stays out
of the assistant's context):

```bash
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -20
```

**Run the suite in parallel (ANTS-2231).** ctest is serial by default
(~78 s full suite); the test presets now parallelise it — `default` /
`fast` at `-j4`, `workstation` at `-j2` (`debug` stays serial: ASan is
~3× RAM; `perf` stays serial: benchmarks must not contend):

```bash
ctest --preset=default          # ~19 s — parallel, perf excluded
ctest --test-dir build -j4      # same, without the preset wrapper
```

`-j4` is the cap tuned for this 32 GiB / earlyoom host — the test
processes are light (unlike parallel `cc1plus`), but keep it ≤4 so a
heavy desktop session doesn't thrash. The full suite is verified green
+ flake-free at `-j4`. Narrower runs stay fastest: `-R <regex>` (one
suite), `-L features` (one label), `--target <bundle>` to build only the
bundle you touched.

### Cheaper iteration loops (ANTS-1550 / ANTS-1552)

- `cmake --build build --target ants-terminal` — skip the ~11 test binaries.
- `-DANTS_TESTS=OFF` — drop test targets from the graph.
- `-DANTS_CCACHE=ON` (default) — ccache compiler launcher; a cache hit
  skips `cc1plus` entirely. **Keep the cache big enough** — at the 5 GiB
  default this Qt codebase fills it and self-evicts (~40% hit rate);
  `ccache -M 20G` once lifts the hit rate so cold-after-pull rebuilds
  reuse far more objects (pure disk, no RAM cost). `ccache -s` to check.
- `-DANTS_USE_MOLD=ON` (default when `mold` is on PATH; ANTS-2233) — links
  with mold instead of GNU ld. Linking is the heaviest, highest-RSS step
  (hence `link_pool=1`); mold is multi-threaded *and* lower-RSS, so it
  shortens the ~30-bundle link tail without raising the OOM ceiling. Auto
  falls back to the default linker when mold is absent (e.g. CI).
- `-DANTS_UNITY_BUILD=ON` — opt-in; viable end-to-end since ANTS-1553.
  Unity applies only to the always-fully-linked libs (chrome / claude /
  dialogs / audit_dialog — the Widgets-heavy cc1plus hogs); the
  subset-linked libs (core / vt / audit / lua) stay per-TU so the test
  bundles' selective `--start-group` links don't drag cross-lib externals
  (esp. core's aggregated AUTOMOC). Best for cold full builds; it
  penalises incremental rebuilds (a one-file edit recompiles its whole
  unity batch), so it is NOT wired into the `fast` preset.
- `cmake --preset=fast` — isolated `build-fast/` dir + `ANTS_LINK_POOL=2`
  (parallel test-bundle linking); ccache + PCH are unconditional defaults
  on every preset, not fast-only.

### CMake presets

Each preset: `cmake --preset=X && cmake --build --preset=X && ctest --preset=X`.

| Preset | Use |
|---|---|
| `default` | Release + Ninja in `build/`; honours the JOB_POOLS cap. |
| `workstation` | Release in `build-workstation/`, hard-capped `-j3` for constrained hardware / heavy desktop sessions. |
| `debug` | Debug + ASan/UBSan in `build-asan/`, sanitizer env wired into `ctest --preset=debug`. |
| `fast` | Release in `build-fast/` with `ANTS_LINK_POOL=2` (parallel test-bundle linking) for hot iteration loops; isolated dir keeps `build/` warm. |

### Backstop: `tools/safe-build.sh`

Wraps `cmake --build` in a systemd-user scope (`MemoryMax=24G` /
`MemorySwapMax=8G`) so a future over-parallelism regression kills the
*build*, not the session. Layers 1–2 (JOB_POOLS, `workstation` preset)
should make it unnecessary; reach for it after kernel / Qt-major updates.
Optional audit deps self-disable if absent. **Cppcheck gotcha:** pass
`--library=qt` or it misparses `emit` as a type. Qt projects ONLY —
elsewhere that flag breaks the parse and the TU loses all coverage
(ANTS-4094).

### Local CI: `tools/ci-parity.sh` + the pre-push hook (ANTS-2134 / 3410 / 3580)

**`tools/ci-parity.sh --full` IS this project's local CI check** — it covers
all three jobs of `.github/workflows/ci.yml` (`build-test` incl. the
packaging/lint gates, `build-asan`, and `qt62-baseline` in a podman
ubuntu:22.04 container). There is no second script; anything calling itself
`local-CI.sh` would be a duplicate of this one. A gate whose tool is absent
SKIPs loudly and is listed as incomplete parity — never silently green.

**It is a hand-maintained PARALLEL IMPLEMENTATION, not a runner for
`ci.yml` (ANTS-4392).** It contains no workflow parser, no `act` and no YAML
read; it re-states the jobs in shell. So the two can drift, and the drift it
cannot catch by construction is anything *declared* in `ci.yml` that the
script never knew to assume — a runner package, an env var, an action
version. **ANTS-4391 is what that costs**: `ripgrep` was installed by no job,
CI was red for five commits, and no local run could see it because rg exists
on the dev box. The repair for that class is a *static* check that the
recipes agree with the source (`tests/features/ci_workflow_deps`), never a
parallel implementation trying harder. **It reads every carrier that runs the
suite, not just `ci.yml`** (ANTS-4717): the workflow, the RPM spec, the Arch
PKGBUILD and the Debian control. Guarding one carrier turns a class defect
into a queue of surfaces, each found by a build — the RPM was the second.
Adding a carrier that runs `ctest` means adding it to that test.

Driving the real workflow with `act` was considered and
rejected — it pulls container images and is slow enough that nobody would run
it before a push, and a gate nobody runs catches nothing.

CI is red where local is green when the runner's environment differs:
`C.UTF-8` POSIX collation (ANTS-2120) and a loaded 4-vCPU host that
exposes timing races (ANTS-2130). Builds + runs in isolated
`build-ci-parity*/` trees so the live `build/` is untouched; `--repeat N`
(ctest `until-fail`) flushes flakes, `--stress` adds CPU load.

**It runs before every push automatically, in reduced form.**
`tools/hooks/pre-push` (wired via `core.hooksPath=tools/hooks`) gates each
push on the Release suite against the warm `build/`, plus — when a
sanitizer tree already exists — an incremental `build-asan` build and its
sanitized suite. Docs-only pushes skip, mirroring `ci.yml`'s `paths-ignore`.
Not covered by the hook: `--lints`, the containerised `qt62-baseline` job,
and `e2e`/`perf`; run `--full` before a release and when touching
packaging- / e2e-sensitive code. **The Qt-floor half IS covered now**
(ANTS-4131): the hook runs `tools/qt62-guard.sh --warm-only`, a compile
guard against the Qt 6.2 floor, whenever a push carries compilable source,
and skips with a message when it does not. So adding a source file no
longer obliges a manual `--qt62` — a green push has already been compiled
against the floor. What that guard still cannot see is anything only the
full ubuntu:22.04 container exercises (packaging, distro Qt behaviour at
runtime).

Why the guard exists: a push that ADDS a TU once cost three consecutive
red CI runs — `spec_conformance` shipped using
`QRegularExpressionMatch::hasCaptured()`, which is **Qt 6.3+** against this
project's **Qt 6.2 floor** (`dependencies.md` § 4). It compiled here, passed
3393/3393, and passed the hook, because at that time only `qt62-baseline`
could see a floor violation and the hook did not run it. A new TU is still
the change most likely to reach for a Qt API newer than the floor; it is
now caught before the push rather than by CI.
Escape hatches: `git push --no-verify`, `ANTS_PREPUSH_NO_ASAN=1`,
`ANTS_PREPUSH_NO_QT62=1`. The ASan
leg is cost-gated (ANTS-4118): it runs only over a tree that is warm
(≤ `ANTS_PREPUSH_ASAN_MAX_EDGES`, default 25 pending steps), skipping with
a loud message when the tree is cold, mid-CMake-regen, or carries a damaged
deps log from a killed build — so a caller's command timeout can no longer
SIGTERM a push mid-ninja.

## Test harnesses

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
  --target` line and the `ctest -R` filter for that source's suites in one
  call, and `found:false` is exactly the not-yet-wired state above. Bundles
  are not guessable from the path — `tests/features/spec_conformance/` builds
  into `test_claude`, not `test_core`. The old recipe (`grep -n <feature>
  CMakeLists.txt`, then read upward for the enclosing `ants_add_*_bundle(`)
  still works and is the fallback outside an Ants session.
- **E2E harness** (`tools/e2e/`, label `e2e`, ANTS-2049) — drive a
  throwaway `--e2e` instance as a user (inject-key/click, resize-window,
  grab-image over its socket via `--remote-json`) and observe it. Opt-in:
  `ctest -L e2e` (excluded from the default presets). Two suites:
  `smoke.sh` (harness-contract guards) and `cases.sh` (ANTS-2050
  feature lanes — `terminal`/`scrollback`/`resize`/`theme`, run all or
  `cases.sh <lane>`). Full feature checklist (auto/manual/pending-hook):
  [`docs/qa/e2e/cases.md`](docs/qa/e2e/cases.md); how-to + case format:
  [`docs/qa/e2e/README.md`](docs/qa/e2e/README.md).

## Conventions

- Signals/slots for cross-component comms.
- Config at `~/.config/ants-terminal/config.json`, mode 0600.
- **The roadmap store is MACHINE-GLOBAL, not per-project**
  (`~/.local/share/ants-terminal/roadmap.sqlite`, mode 0600). Anything
  registered in it outlives the session that registered it and is summed by
  every `scope:"all"` surface. Two consequences worth knowing before you touch
  it. **`roadmap_migrate` refuses a root under the system temp dir**
  (`transient_root`, ANTS-4600, `RoadmapMigrateVerb::isTransientRoot()`) —
  a session scratchpad once got registered, survived `registerProject()`'s
  INV-8 because it still existed at migration time, and left 33 duplicate
  items behind under a path deleted minutes later. That guard is in the
  HANDLER, not in `run()`: `run()` takes an arbitrary `storePath` and
  ANTS-3855's fixtures legitimately migrate temp roots into temp stores, so
  a guard inside it reddens the suite. And **the schema declares
  `REFERENCES` but no `ON DELETE CASCADE`**, so deleting a project means
  deleting element → history → feedback_ref → relationship → citation →
  message → item → section → id_prefix → project by hand, in that order,
  with `PRAGMA foreign_keys = ON`. **Two of those ten clear BOTH ends** —
  `relationship` and, since ANTS-4622, `message`: each names two projects, so
  a row in another project pointing into this one would otherwise dangle.
  **Back it up with sqlite3 `.backup`, never `cp`** — the store runs in WAL
  and a live Ants holds a connection.
  **A `kSchemaVersion` bump is a ONE-WAY DOOR across every project on the
  machine** (ANTS-4462, 2026-08-24). `RoadmapStore::open()` refuses outright
  when the store's `user_version` exceeds the build's — "store schema N is
  newer than this build's M", returning false, which makes every roadmap verb
  refuse. Because the store is machine-global, the FIRST binary to upgrade it
  locks every older build out of every project in it: an older AppImage, a
  Patron RC, another checkout still on the previous version. So a new column
  is never merely additive here, whatever `ALTER TABLE ... DEFAULT` suggests.
  Before reaching for a rung, ask whether the value can be DERIVED instead —
  ANTS-4462 wanted a `store_synced_at` column and got the same answer by
  rendering and comparing, which needed no migration, cost nothing to
  withdraw, and was strictly more correct (a stored stamp records what the
  store *believes* it published, so an edit after that stamp is invisible to
  it). Measure first: one render of a 2,267-item project is ~204 ms, which is
  too slow per-query and perfectly fine for an opt-in check.
- Per-project layout is optionally declared in a repo-committed
  `<root>/.ants/project.json` (ANTS-2160, `src/projectsettings.cpp`):
  `source_roots`/`test_roots` (codebase_index), `docs_dir`,
  `roadmap`, `changelog`, `specs_dir` — consumed by codebase_index /
  docs_index / roadmap_query / changelog_log / spec_query+log /
  current_state / project_layout, each falling back to its heuristic
  when a key is absent. Distinct from the global config above:
  per-project, world-readable, no secrets (NOT mode 0600). ANTS-2161
  adds the write side: `session_orient` emits a
  `project_settings_suggestion` when a project's code isn't under
  `src/` (gated on a near-empty `codebase_index`), and the
  `project_settings` verb (`detect`/`init`/`set`) creates & updates the
  file in one call.
- Scrollback default 50k, max 1M.
- Theme colors set on `TerminalGrid`; ANSI palette (16+216+24) lives there.
- QTextLayout for ligature shaping.

## MCP tool authoring

**Discovering the full toolkit (ANTS-2037).** The SessionStart hook lists
~12 high-frequency verbs; for the full set (grouped by category, each with a
one-line *when to use*, and the live `tool_count`), call
`tool_info {catalog:true}` (ANTS-1985) once,
then `ToolSearch` `select:mcp__ants__<name>` to load a verb's schema before
calling it. This pointer is the always-loaded fallback for when the hook
prelude is stale (ANTS-2038) or disabled.

**Session bootstrap refreshes the codebase map (ANTS-2140).**
`session_orient` (the documented first call) embeds + eagerly
refreshes `codebase_index` (ANTS-1637), so the codebase map is
rebuilt at session start. Query it via `codebase_index` /
`find_definition` / `find_sources` / `workspace_search` rather than
`grep` (cheaper, and it's the index the first call just refreshed).

When adding or modifying an MCP tool, follow
[`docs/standards/mcp-tools.md`](docs/standards/mcp-tools.md) (the umbrella
checklist; its *Load-bearing contracts* quick-reference lists each
contract — response-wrap, caller_cwd, CallerCwdContract, path validation,
ETag-304, `fields=`, refusal codes, state routing — with its ANTS-spec).
Per-verb behavioural reference (the `get_scrollback` / `roadmap_query` /
`read_region` / `codebase_index` / `apply_edits` / `model_switch_stats` …
notes) lives in
[`docs/standards/mcp-behavioural-notes.md`](docs/standards/mcp-behavioural-notes.md).
Both were moved out of this preamble by ANTS-2088 — read on demand; the
live verb catalogue + one-line *when to use* per verb is `tool_info
{catalog:true}`.

**MCP config keys — catalogued in
[`docs/standards/mcp-config-keys.md`](docs/standards/mcp-config-keys.md)**
(ANTS-3429; moved out of this preamble like ANTS-1292/2088). The master gate is
`claude.mcp_enabled` (ANTS-1901, Settings → General; when false the socket
isn't bound at launch, the auto-switcher stands down, and every verb refuses
`mcp_disabled`). The doc also covers the autonomous model-switcher keys
(ANTS-1735), result-offload keys (ANTS-2094), the per-call `encoding:"tabular"`
columnar arg (ANTS-2090), and `project_query` keys (ANTS-2093) — read on demand.

## Cross-session MCP feedback

Other CC sessions (Vestige, MAME Curator, Album Builder, RetroArch,
RetroDB, and others) write MCP observations to `*_Ants_MCP_Feedback.md` files under
`/mnt/Games/Scripts/Linux/`. Format spec:
[`docs/standards/mcp-feedback-files.md`](docs/standards/mcp-feedback-files.md).

> **v2 is live (2026-07-10).** The format moved to an inline-ID model — the
> maintainer fills each finding's `**Proposed ID:**` slot in place
> (`op:assign_id`) and status is derived live from the ROADMAP, so v2 **stops
> writing tracking tables**. All ten corpus files are migrated to `: 2` (their old
> v1 tables are retained in place pending a declutter pass). `op:append_tracking` and
> the v1 table-compaction ops below are **legacy** (un-migrated files only); the
> canonical v2 flow is `migrate_v2` → `assign_id` → `compact_resolved`.

At session start, `session_orient` surfaces a `feedback_pending` block
(ANTS-1964) — a per-file count of un-triaged contributor addenda across the
shared-root files, so you see which need triage without one `feedback_query`
per file. Only the Ants maintainer project gets it (gated on shipping
`docs/standards/mcp-feedback-files.md`); only files with pending input list.

Reviewing feedback efficiently (don't re-read the whole file):
- **`feedback_query`** (ANTS-1961) — pass the feedback file's `path`;
  returns the un-triaged tail + already-mapped `ANTS-NNNN` IDs; saves ~60k
  tokens vs. a full read. Version-aware (ANTS-3448): on a `: 2` file the tail is
  the findings whose `**Proposed ID:**` is still unfilled; on a v1 file it's the
  contributor blocks after the last maintainer tracking table. Read-only, ETag-aware.
- **`feedback_log`** (ANTS-1962) — write side (`path` basename must end in
  `_Ants_MCP_Feedback.md`). Contributor: `op:"append_finding"` (append-only at
  EOF; creates the skeleton first time; stamps a blank `**Proposed ID:**` line).
  Maintainer v2 triage: `op:"assign_id"` (ANTS-3447, fill the id/closure slot),
  `op:"compact_resolved"` (ANTS-3443, collapse shipped write-ups),
  `op:"migrate_v2"` (ANTS-3446, +`backfill_from_tracking` ANTS-3474, convert a v1
  file). **Legacy (v1 only):** `op:"append_tracking"`, `op:"compact_shipped"`
  (ANTS-3421), `op:"prune_tracking"` (ANTS-3442). Per-op detail: the standard's §Tooling.

Triage flow (v2): `feedback_query` the tail → allocate IDs via `roadmap_log
op:append` → `feedback_log op:"assign_id"` to fill each finding's
`**Proposed ID:**` slot (or an `n/a — <reason>` closure) → once an ID ships,
`op:"compact_resolved"` collapses the write-up. Filling the slot removes that
finding from the next `feedback_query` delta (no watermark to advance).

## Project standards

**The shared standards are owned globally, at `~/.claude/standards/`.**
`coding.md`, `documentation.md`, `testing.md` and `commits.md` in
`docs/standards/` are **deltas**: each opens with the Qt/C++/Ants-specific
rules that cannot live in a language-agnostic standard, then mirrors its
owner verbatim below a divider (`security.md` is the mirror alone, no delta).
Read the delta half for what this project adds.

**Never edit a mirrored half.** A correction goes upstream, then
`tools/check-standard-mirrors.sh --write` re-copies it down;
`tools/hooks/pre-commit` refuses a commit whose mirror has drifted
(ANTS-4133). The mirrors exist because this repo is public and an outside
reader cannot open a path in a private home directory — not as a licence to
keep a second copy of a rule. That was the pre-2026-08-12 arrangement, where
`/start-app` copied the set in with an instruction to keep them verbatim and
nothing checked it: they drifted for three months and three ended up
instructing behaviour the owner forbids.

**Check the owner is COMMITTED before running `--write`.** `~/.claude` is a
live repo another session may be editing, and the gate compares against the
owner's working tree, not its HEAD — so a review pass in flight upstream
shows up here as drift on a file you never touched, and `--write` copies a
half-written document into this public repo. `git -C ~/.claude status
--porcelain` first: if the owner is dirty, the honest move is to leave that
mirror alone and commit with `ANTS_PRECOMMIT_NO_MIRRORS=1`, saying so in the
message. Where the owner is clean, `--write` is right and the drift is real
(its history will usually name the pass that changed it). Hit 2026-08-14 on
`security.md` while `documentation.md`, committed upstream the same hour,
needed the ordinary re-copy — two mirrors, two different correct answers,
minutes apart.

**Read the drift diff's direction right: `<` is the OWNER, `>` is the project
mirror.** The header says "docs/standards/X.md: DRIFTED from
~/.claude/standards/X.md", which reads as if the project file is the first
operand and therefore the `<` side. It is not. Get this backwards and the
mirror looks *ahead* of its owner — a state that should be impossible — and
the tempting repair is to edit the owner, i.e. push a mirror's text upstream,
which is the one direction the whole arrangement forbids. Confirmed 2026-08-14
by opening the owner and finding the `<` text in it, after exactly that
misreading; if in doubt, do the same rather than reasoning from the header.

**Two files are NOT deltas, deliberately:**

- [`roadmap-format.md`](docs/standards/roadmap-format.md) — **this project
  is UPSTREAM of the global copy** (CFG-0069, user decision 2026-08-12),
  because the parser, the store and the migration all live here. Where the
  two disagree, this one governs and the global copy is corrected to match —
  never the reverse. It is a full standard and stays one.
- [`specs.md`](docs/standards/specs.md) — a full standard, not a delta; its
  § 0 records why. It owns a spec's **shape**; global `spec-format.md` § 1
  owns whether a spec is needed at all.

Project sub-specs. The list below is illustrative, not the roster —
[`docs/standards/README.md`](docs/standards/README.md) is the index, and a
file there binds you whether or not it is named below, unless it marks itself
superseded (as `mcp-errors.md` does). Most have no global counterpart;
`dependencies.md` is the exception and its entry says so:

- [`mcp-error-codes.md`](docs/standards/mcp-error-codes.md) (ANTS-1353) —
  canonical `code` taxonomy for refusal envelopes.
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
  global `standards/dependencies.md` (which carries the policy and no
  version numbers).
- [`mcp-config-keys.md`](docs/standards/mcp-config-keys.md) (ANTS-3429) —
  Ants-MCP config-file / Settings keys (master gate, auto model-switcher,
  result offload, tabular encoding, `project_query`); relocated from this
  preamble to keep the always-loaded session context lean.

**Do not cite a section number of a delta file from memory.** The global
files were restructured on 2026-08-12 and the numbers moved: the
symbol-not-line-numbers rule is global `documentation.md` § 2.3 (it was
§ 1.7 in the old project copy), and global `coding.md` now has its own
§ 1.6 / § 1.7 on entirely different subjects. Open the file.

[`mcp-errors.md`](docs/standards/mcp-errors.md) is a superseded
(2026-05-12) draft — `mcp-error-codes.md` is the authoritative taxonomy.

ADRs live at `docs/decisions/` (Nygard format); per-feature specs at
`docs/specs/`; per-phase outcomes at `docs/journal/`. `docs/plans/` is
deprecated (historical records only).

## Versioning & release

SemVer. **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single
source of truth** — `ANTS_VERSION` propagates everywhere; never hardcode
versions in `.cpp` / `.h`. Every bump touches `CMakeLists.txt` and
`README.md` ("Current version") — the `CHANGELOG.md` section is rolled by
`new-rc` and dated by `promote`, never by the bump (see below); use
`cut-release --bump-only` (its `.claude/bump.json` covers the packaging
files). **That skill replaced both
`/bump` and `/release`, which were deleted 2026-08-13** — the old names are
gone, not aliased, so a session invoking them gets nothing. Every cycle
also **re-checks README.md's prose, not just its version banner** — a
`bump.json` todo owns the criteria; update it only when a user-visible
claim has actually drifted (the tool count is the one
nothing else verifies). Completed `ROADMAP.md` items reach the CHANGELOG as
an authored summary — the copy rule below owns what that means.
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
release, use
**`cut-rc.sh hotfix <fix-sha>…`** (ANTS-2165): two phases around a
bump that ship `vN` + the cherry-picked fix as the next public patch
and roll the in-flight RC up one number.

**`new-rc` builds and tests before it tags — run it backgrounded, never
under a short command timeout.** The preceding bump edits
`CMakeLists.txt`, which regenerates `build_info` and invalidates most of
the graph, so the gate is a near-full rebuild (630 pending steps on
0.7.105). A rehearsal killed at a 2-minute timeout is a SIGTERM'd ninja —
the corruption case CLAUDE.md warns about elsewhere. It survived
(`ninja -C build -n` exit 0, `-t recompact` clean), but check that before
doing anything else if it happens. `--skip-build` skips the gate, not the
tagging. **Never leave the bump to author a CHANGELOG section** —
`.claude/bump.json` todo 2 explains why; `new-rc` owns that roll.

**Check the roadmap store for shipped-but-unrecorded work at bump time**
(ANTS-4714): `bash tools/check-shipped-coverage.sh` lists every item the
store says shipped since the last public tag that no CHANGELOG bullet
cites. It is the CONVERSE of the release skill's own gate, which only
checks that ids the CHANGELOG *claims* are really shipped — that direction
cannot see work that shipped and was never written down. Run it BEFORE
`new-rc` rolls `[Unreleased]`, because afterwards a missing entry has to go
into a closed section. It reads `roadmap.sqlite` directly, and keeps doing
so: `roadmap_query` CAN now return the ids (`shipped_since` on the list
path, ANTS-4715), but only to a session talking to a running instance — a
shell script reaches a verb through `--remote-json`, which needs one. That
would trade a dependency on `sqlite3` for a dependency on the GUI app being
up, and the gate must work headless (ANTS-4734). The store stamps `shipped` only
from 2026-08-20 forward, so every run also reports how many shipped items
carry no date and were invisible to it; `roadmap_log op:"backfill_dates"`
fills those from git history.

**A CHANGELOG entry states what SHIPPED, never what was wrong — do not copy a
roadmap headline that states a PROBLEM into the bullet** (ANTS-4759). A defect
item's headline states one, so copying it puts the bug in the release notes
where the fix belongs: `### Fixed` ends up announcing "mutation_probe keeps
mutating the source after the transport has timed out". `releases.md` § 2 makes the
changelog section the description of what shipped, and that is the line this
breaches. It is a REFLEX, not a one-off — 14 of 56 `[Unreleased]` bullets were
verbatim copies on 2026-08-31, written across seven separate commits, and
`changelog_log op:"add_from_roadmap"` produces one by design (ANTS-1868 fixed
only the line-wrapping half). So prefer `op:"add"` with an authored summary
wherever the headline states a problem — which is not only the `fix` kinds: 6
of the 14 were `enhancement`, filed as the gap they closed. Keep
`add_from_roadmap` for an item whose headline already reads as a delivered
change. `tools/check-shipped-coverage.sh` reports the copies alongside the
uncovered items and **exits non-zero on either** — it is `cut-rc.sh` that
chooses not to block, so do not wire the bump-time run into a `set -e` chain
expecting it to pass. It compares **byte-identically**, so a reworded headline
that still states the problem is not caught: check those yourself.

## Key design decisions (non-obvious)

- Custom VT100 parser, no pyte/libvterm. Qt6 is the only runtime dep.
- Delayed-wrap (xterm-style) for correct line wrapping.
- Alt-screen 1049 supported (vim/htop).
- Combining chars in per-line side table — zero overhead when absent.
- Image paste auto-saves and inserts the filepath (Claude Code workflow).
- Lua sandbox strips dangerous globals + instruction-count timeout.
- Session persistence via `QDataStream` + `qCompress`.
- `opacity` config drives per-pixel terminal-area fillRect alpha only;
  chrome paints opaque (`WA_StyledBackground`). No `setWindowOpacity()`
  path (`background_alpha` removed as redundant in 0.7.18).
- Audit rule pack is JSON not YAML (`QJsonDocument` built-in). Hardcoded
  checks stay in C++; `audit_rules.json` only appends/overrides.
- Audit uses `clazy-standalone` (Qt-aware AST), not embedded libclang.
- `.audit_suppress` is JSONL v2 (`{key, rule, reason, timestamp}`); v1
  plain-key lines load and convert on first write.
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
