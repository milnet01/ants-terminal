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

**Never relink `build/` while an instance is running (ANTS-2025).** The user
launches `build/ants-terminal` (Plasma icon → `launch.sh`), and the linker
rewrites that file in place, so a running instance later demand-pages a
corrupted code page and SIGSEGVs. During a live session build to the isolated
`build-fast/` tree (`cmake --build build-fast`, or `tools/build-and-stage.sh`
which also atomically swaps the result into `build/`); `launch.sh` promotes a
newer `build-fast/` binary into `build/` via an atomic `rename(2)` on the next
launch — a running instance keeps its old inode. The binary must run from
`build/` (it resolves assets + the MCP project root via `applicationDirPath`),
so the file is swapped, not relocated.

**Token-frugal invocations** (pipe to `tail` so a 10k-line log stays out
of the assistant's context):

```bash
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -20
```

### Cheaper iteration loops (ANTS-1550 / ANTS-1552)

- `cmake --build build --target ants-terminal` — skip the ~11 test binaries.
- `-DANTS_TESTS=OFF` — drop test targets from the graph.
- `-DANTS_CCACHE=ON` (default) — ccache compiler launcher; a cache hit
  skips `cc1plus` entirely.
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

Master Ants-MCP gate (ANTS-1901) — sits hierarchically above every
per-feature key below: `claude.mcp_enabled` (bool, default true),
Settings → General → "Enable Ants MCP integration". When false, the MCP
socket isn't bound at launch, the orientation hook is removed, the
auto-switcher stands down, and every verb refuses with `mcp_disabled`
(turning it off is honoured immediately via the dispatcher guard;
turning it back on takes effect on the next launch).

Config keys for the autonomous model switcher (ANTS-1735 §2.7) — single
Settings toggle "Let Ants pick the Claude model for me" + two
config-file-only tuning keys:

- `claude.auto_model_switch` (bool, default false) — master gate.
- `claude.auto_model_min_dwell_sec` (int, default 90, clamp [30, 1800]).
- `claude.auto_model_floor` (`"haiku"`|`"sonnet"`, default `"haiku"`).
- `claude.auto_model_nudge_shown` (bool, default false) — first-run
  opt-in nudge latch (§8 OQ-3).
- `claude.auto_model_toast_enabled` (bool, default true) — ANTS-1893
  switch-event surfacing: status-bar toast on each live auto-switch.
- `claude.auto_model_chip_pulse_enabled` (bool, default true) —
  ANTS-1893: per-tab model chip pulses for ~0.6 s on each switch.
- `claude.auto_model_undo_enabled` (bool, default true) — ANTS-1893:
  10 s "Undo: back to <Tier>" button in the status bar after each
  switch; click seeds the ANTS-1890 cool-down so the same pick
  won't immediately re-fire.
- `claude.auto_model_confirm_user_switch` (bool, default true) —
  ANTS-1951: auto-confirm CC's "Switch model?" dialog for user-typed
  `/model` commands too (the auto-switch/chip/undo paths already
  confirm via `performModelSwitchHandshake`). Runs on the 2 s tick
  independently of the master gate; stands down while an Ants-initiated
  handshake owns the dialog; sends only ENTER (no continuation prompt).
- `claude.auto_model_debug` (bool, default false) — ANTS-1976: toggleable
  per-tick switcher trace. When true, force-enables the DebugLog
  `autoswitch` category so every gate evaluation + switch decision is
  logged to `debug.log` (state, composerEmpty, composerStaleMs,
  toolUseMs, current/target tier, act, tier, blockedBy[], reason, epoch).
  Env equivalent: `ANTS_DEBUG=autoswitch`. Off = single bit-test, no cost.

Config keys for result offload (ANTS-2094, observation masking) — when a
large read result is spilled to a content-addressed cache file and a
`{offloaded:true, handle, head, …}` envelope returned instead; re-read the
full body via the `read_spill` verb. Config-file-only:

- `claude.mcp_offload_large_results` (bool, default **false**) — session
  default for the per-call `offload` arg (per-call wins). OFF for v1
  because it changes the response contract; flips ON in a fast-follow once
  `read_spill` is field-proven.
- `claude.mcp_offload_threshold_bytes` (int, default 16384, clamp
  `[4096, 1048576]`) — minimum final body size to offload.
- `claude.mcp_offload_head_bytes` (int, default 2048, clamp `[256, 16384]`)
  — preview head size; offload only fires when the body also exceeds this
  (spilling something that fits in the head saves nothing).

## Cross-session MCP feedback

Other CC sessions (Vestige, MAME Curator, Album Builder, RetroArch,
RetroDB) write MCP observations to `*_Ants_MCP_Feedback.md` files under
`/mnt/Games/Scripts/Linux/`. Format spec:
[`docs/standards/mcp-feedback-files.md`](docs/standards/mcp-feedback-files.md).

At session start, `session_orient` surfaces a `feedback_pending` block
(ANTS-1964) — a per-file count of un-triaged contributor addenda across the
shared-root files, so you see which need triage without one `feedback_query`
per file. Only the Ants maintainer project gets it (gated on shipping
`docs/standards/mcp-feedback-files.md`); only files with pending input list.

Reviewing feedback efficiently (don't re-read the whole file):
- **`feedback_query`** (ANTS-1961) — pass the feedback file's `path`;
  returns the un-triaged tail (contributor blocks after the last maintainer
  tracking block) + already-mapped `ANTS-NNNN` IDs; saves ~60k tokens vs. a
  full read. Read-only, ETag-aware.
- **`feedback_log`** (ANTS-1962) — write side; `op:"append_finding"` for
  contributors, `op:"append_tracking"` for the maintainer to stamp a
  mapping table with roadmap IDs. Append-only at EOF (creates the file with
  a conforming skeleton on first `append_finding`). The `path` basename
  must end in `_Ants_MCP_Feedback.md` (else `not_feedback_file`).

Triage flow: `feedback_query` the tail → assign IDs via `roadmap_log
op:append` → `feedback_log op:"append_tracking"` to stamp the mapping
table (which advances the watermark, emptying the next `feedback_query`
delta).

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

Project-local additions: `coding.md` adds the `setOwnerOnlyPerms()` note
(§5.2); `documentation.md` adds §7 Accessibility (ANTS-1235);
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
