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
(see `remotecontrol.cpp:8129` — applicationDirPath would be the build dir, which
is wrong); all user state lives under XDG paths.

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
`--library=qt` or it misparses `emit` as a type.

### CI parity: `tools/ci-parity.sh` (ANTS-2134)

CI is red where local is green when the runner's environment differs:
`C.UTF-8` POSIX collation (ANTS-2120) and a loaded 4-vCPU host that
exposes timing races (ANTS-2130). `tools/ci-parity.sh` builds + runs the
suite under `LC_ALL=C.UTF-8` in an isolated `build-ci-parity/` tree;
`--repeat N` (ctest `until-fail`) flushes flakes, `--stress` adds CPU
load. Quick check: `LC_ALL=C.UTF-8 ctest --test-dir build`.

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
~12 high-frequency verbs; for all ~73 (grouped by category, each with a
one-line *when to use*), call `tool_info {catalog:true}` (ANTS-1985) once,
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

Shared v1 standards in `docs/standards/` (from the `/start-app` template):
[`coding.md`](docs/standards/coding.md),
[`documentation.md`](docs/standards/documentation.md),
[`testing.md`](docs/standards/testing.md),
[`commits.md`](docs/standards/commits.md). Project sub-specs:

- [`roadmap-format.md`](docs/standards/roadmap-format.md) — `[ANTS-NNNN]`
  IDs from `.roadmap-counter`, status emojis (✅ 🚧 📋 💭),
  position-is-priority, `Kind:` / `Source:` taxonomy, `Layman:` field,
  archive rotation.
- [`specs.md`](docs/standards/specs.md) (ANTS-1728) — spec-authoring
  standard for `docs/specs/ANTS-NNNN.md` (H1 + Status/Kind/Source header;
  Problem / Surface / Invariants / Tests; the `- **INV-N** —` bullet form;
  cold-eyes loop log; `spec_query` contract).
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
  Lua 5.4, C++20) are distinct from pins. The project mechanism for global
  `~/.claude/CLAUDE.md` §5.
- [`mcp-config-keys.md`](docs/standards/mcp-config-keys.md) (ANTS-3429) —
  Ants-MCP config-file / Settings keys (master gate, auto model-switcher,
  result offload, tabular encoding, `project_query`); relocated from this
  preamble to keep the always-loaded session context lean.

Project-local additions: `coding.md` adds the `setOwnerOnlyPerms()` note
(§5.2); `documentation.md` adds §7 Accessibility (ANTS-1235) and
§§ 1.6–1.7 (concision; symbol-not-line citations);
`commits.md` / `testing.md` are template-identical.
[`mcp-errors.md`](docs/standards/mcp-errors.md) is a superseded
(2026-05-12) draft — `mcp-error-codes.md` is the authoritative taxonomy.

ADRs live at `docs/decisions/` (Nygard format); per-feature specs at
`docs/specs/`; per-phase outcomes at `docs/journal/`. `docs/plans/` is
deprecated (historical records only).

## Versioning & release

SemVer. **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single
source of truth** — `ANTS_VERSION` propagates everywhere; never hardcode
versions in `.cpp` / `.h`. Every bump touches `CMakeLists.txt`,
`CHANGELOG.md` (new dated Keep-a-Changelog section), `README.md`
("Current version"); use `/bump` (its `.claude/bump.json` covers the
packaging files). Completed `ROADMAP.md` items migrate to CHANGELOG.
Update `PLUGINS.md` in the same commit when the `ants.*` Lua surface
changes.

**Release candidates (ANTS-1318).** The weekly Wednesday cadence cuts a
public release + a Patron-preview RC. The `-rcN` suffix lives ONLY at the
git tag, GitHub-release title, and AppImage filename — never in
`CMakeLists.txt` / `bump.json` (INV-3 / INV-9). RC orchestration is
`packaging/cut-rc.sh` (`new-rc` / `respin` / `promote` / `status` /
`cycle` / `hotfix`), NOT the global `/release` skill. Flow: `/bump` to
base `X.Y.Z`, then `cut-rc.sh new-rc --push`. `release.yml` routes RC
AppImages to a separate zsync channel so stable users can't auto-update
onto an RC.

The **guarded Wednesday cadence is `cut-rc.sh cycle`** (ANTS-2164):
promote the in-flight RC, then cut the next one — each phase self-skips
when there is nothing to do and hard-refuses an empty / placeholder /
stale / drifted RC; `new-rc` auto-rolls `[Unreleased]` and `promote`
auto-date-stamps the CHANGELOG/metainfo/debian carriers. The bump
between phases is still a separate `/bump` (the script never edits
version files). For an urgent bug in the already-published release, use
**`cut-rc.sh hotfix <fix-sha>…`** (ANTS-2165): two phases around a
`/bump` that ship `vN` + the cherry-picked fix as the next public patch
and roll the in-flight RC up one number.

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
