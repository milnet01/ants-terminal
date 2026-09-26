# Ants Terminal

Qt6/C++20 terminal emulator. Optional Lua 5.4 plugins. `libutil` for PTY.
CMake build.

## Module map (src/)

Per-subsystem reference: [`docs/subsystems.md`](docs/subsystems.md). Query it
with the `subsystem` MCP tool (`op=map`, `op=files`, `op=recent_changes`). Keep
it in sync with the code.

**Review lanes come from `.indie-review/partition.json`, not
`docs/subsystems.md`.** To change what a lane covers, edit that file.

## Data flow

`PTY → VtParser → TerminalGrid → TerminalWidget`
Reverse (DA/CPR/DSR): `TerminalGrid → ResponseCallback → PTY`

## Build & test

```bash
cmake -G Ninja -B build && cmake --build build && ctest --test-dir build --output-on-failure -LE 'perf|e2e'
```

- Tests build by default; `-DANTS_TESTS=OFF` builds the main exe only.
- **Use Ninja, not Make.** The `JOB_POOLS` cap in `CMakeLists.txt` applies
  only under Ninja.
- **Rebuild `build/` freely during a live session.** `launch.sh` runs a copy
  under `${XDG_DATA_HOME:-~/.local/share}/ants-terminal/bin/`. Do not
  pre-check `pgrep` before a build.
- Pipe build and test output through `tail` to keep it out of context.
- Run the suite through a preset: `ctest --preset=default` (`-j4`; keep it at
  4 or below on this host). Narrow with `-R <regex>`, `-L features`, or build
  one `--target <bundle>`.

### Faster loops

- `cmake --build build --target ants-terminal` skips the test binaries.
- `-DANTS_CCACHE=ON` (default). Keep the cache large: `ccache -M 20G` once;
  `ccache -s` to check.
- `-DANTS_USE_MOLD=ON` (default when `mold` is on PATH).
- `-DANTS_UNITY_BUILD=ON`: cold full builds only.
- `cmake --preset=fast`: isolated `build-fast/`, parallel test-bundle linking.

### CMake presets

Each: `cmake --preset=X && cmake --build --preset=X && ctest --preset=X`.

| Preset | Use |
|---|---|
| `default` | Release + Ninja in `build/`. |
| `workstation` | Release in `build-workstation/`, capped `-j3`. |
| `debug` | Debug + ASan/UBSan in `build-asan/`, serial tests. |
| `fast` | Release in `build-fast/` for hot iteration. |

### Backstop: `tools/safe-build.sh`

Wraps `cmake --build` in a memory-capped systemd scope. Use it after kernel
or Qt-major updates. **Cppcheck:** pass `--library=qt`, on Qt projects only.

### Local CI and the pre-push hook

- **`tools/ci-parity.sh --full` is this project's local CI.** It executes
  `.github/workflows/ci.yml`'s jobs through `tools/ci_workflow.py`. There is
  no second script. `--stress` adds CPU load.
- Hunt a flaky test with `ctest --test-dir build --repeat until-fail:5 -R <test>`.
- `tools/hooks/pre-push` (wired via `core.hooksPath=tools/hooks`) runs
  `ci.yml`'s `build-test` job, its `build-asan` job when `build-asan/` is
  warm, and two compile guards: `tools/qt62-guard.sh --warm-only` (the Qt 6.2
  floor) and `tools/qt62-guard.sh --job build-test` (ubuntu:24.04's GCC 13 and
  mold). Warm the second once by running it without `--warm-only`.
- A push that `ci.yml`'s `paths-ignore` treats as docs-only skips the hook.
  `--full` runs every job, including those the hook leaves to GitHub.
- A tool the GitHub runner lacks cannot be caught locally.
  `tests/features/ci_workflow_deps` checks the recipes statically. **A new
  carrier that runs `ctest` must be added to that test.**
- Escape hatches: `git push --no-verify`, `ANTS_PREPUSH_NO_ASAN=1`,
  `ANTS_PREPUSH_NO_QT62=1`, `ANTS_PREPUSH_NO_UBUNTU24=1`. The ASan leg's
  contract: `tests/features/prepush_asan_gate/spec.md`.

## Test harnesses

- **`audit_rule_fixtures`** — `tests/audit_self_test.sh` matches rule regexes
  against `tests/audit_fixtures/<rule>/{bad,good}.*`. Count-based.
- **Feature-conformance** (`tests/features/*`, label `features`) — each subdir
  pairs `spec.md` with a test compiled into a shared bundle
  (`tests/features/README.md`). To add one: write `spec.md` first and surface
  it for sign-off; write `test_<feature>.cpp`; add it to a bundle's `SOURCES`
  (never `add_executable`); verify it fails against pre-fix code.
  **Then build that bundle's target and check `ctest -N -R <name>` moved** —
  building the wrong target passes silently. `build_target_for` names the
  bundle; it is not guessable from the path.
- **Perf** (`tools/perf-report.sh`, label `perf`) — not in the presets or CI.
  See [`docs/qa/perf-harness.md`](docs/qa/perf-harness.md).
- **E2E** (`tools/e2e/`, label `e2e`) — `ctest -L e2e`. See
  [`docs/qa/e2e/README.md`](docs/qa/e2e/README.md) and
  [`docs/qa/e2e/cases.md`](docs/qa/e2e/cases.md).

## Hot reload is the design default (user standing rule)

**Every new feature states how it reaches a running terminal without a
relaunch.** Acceptable answers: nothing; a re-read of a file; a client
reconnect. Where "restart the terminal" is unavoidable, the design says so
and why. A relaunch kills every Claude Code session in its tabs (`~Pty`,
`src/ptyhandler.cpp`).

- Behaviour that is data — a rule pack, schema, description, theme, keymap,
  template — lives in a file the process re-reads.
- A verb that needs no tab or terminal state adds no new call into
  `MainWindow` from `src/remotecontrol*.cpp` or `src/claudeintegration.cpp`.
  One that does names the method in its design and reuses, or adds to, the
  enumeration in ANTS-4932.
- `CallerCwdContract::TabSpecific` is not that register. It covers per-tab
  reads routed by `tab` or `caller_cwd`; adding a verb to it switches on a
  refusal.
- A new module states its reload story in its design.

It does not apply to a bug fix, a test, a document or a version bump.
Retrofitting is a roadmap item, never an obligation. Models to copy:
`ants-mcpd` ([`docs/specs/ANTS-4932-standalone-mcp-server.md`](docs/specs/ANTS-4932-standalone-mcp-server.md)),
`config.json`, `audit_rules.json`, the Lua sandbox.

## Conventions

- Signals/slots for cross-component comms.
- Config at `~/.config/ants-terminal/config.json`, mode 0600.
- **The roadmap store is machine-global**
  (`~/.local/share/ants-terminal/roadmap.sqlite`, mode 0600):
  - `roadmap_migrate` refuses a root under the system temp dir. The guard is
    in the handler, not in `RoadmapMigrateVerb::run()`.
  - There is no `ON DELETE CASCADE`. Deleting a project means deleting
    element → history → feedback_ref → relationship → citation → message →
    item → section → id_prefix → project, in that order, with
    `PRAGMA foreign_keys = ON`. `relationship` and `message` clear both ends.
  - Back it up with sqlite3 `.backup`, never `cp`.
  - A `kSchemaVersion` bump locks every older build out of every project.
    Ask first whether the value can be derived instead.
- Per-project layout: optional `<root>/.ants/project.json` (`source_roots`,
  `test_roots`, `docs_dir`, `roadmap`, `changelog`, `specs_dir`).
  World-readable, no secrets. Edited by `project_settings`.
- Scrollback default 50k, max 1M.
- Theme colours are set on `TerminalGrid`, which holds the ANSI palette.
- QTextLayout for ligature shaping.

## MCP tool authoring

- Full verb catalogue: `tool_info {catalog:true}`, then `ToolSearch`
  `select:mcp__ants__<name>` for a schema.
- `session_orient` refreshes `codebase_index`. Query it (`codebase_index`,
  `find_definition`, `find_sources`, `workspace_search`) rather than `grep`.
- Adding or changing a verb: follow
  [`docs/standards/mcp-tools.md`](docs/standards/mcp-tools.md). Per-verb notes:
  [`docs/standards/mcp-behavioural-notes.md`](docs/standards/mcp-behavioural-notes.md).
  Config keys, including the master gate `claude.mcp_enabled`:
  [`docs/standards/mcp-config-keys.md`](docs/standards/mcp-config-keys.md).

## Cross-session MCP feedback

Other Claude Code sessions write `*_Ants_MCP_Feedback.md` files under the
shared root. Format: [`docs/standards/mcp-feedback-files.md`](docs/standards/mcp-feedback-files.md).
`session_orient`'s `feedback_pending` lists files with untriaged findings.

Triage: `feedback_query` the tail → `roadmap_log op:append` →
`feedback_log op:"assign_id"` (or an `n/a — <reason>` closure) → once the id
ships, `feedback_log op:"compact_resolved"`.

## Project standards

- **The shared standards are owned at `~/.claude/standards/`.**
  `coding.md`, `documentation.md`, `testing.md` and `commits.md` in
  `docs/standards/` are deltas: project rules first, then a verbatim mirror of
  the owner below a divider. `security.md` is the mirror alone.
- **Never edit a mirrored half.** Fix the owner, then run
  `tools/check-standard-mirrors.sh --write`. `tools/hooks/pre-commit` refuses a
  drifted mirror.
- **Check the owner is committed before `--write`:**
  `git -C ~/.claude status --porcelain`. If it is dirty, leave the mirror and
  commit with `ANTS_PRECOMMIT_NO_MIRRORS=1`, saying so in the message.
- **In the drift diff, `<` is the owner and `>` is the mirror.** Never push
  mirror text upstream.
- Two files are not deltas. `roadmap-format.md`: this project is upstream of
  the global copy, and this one governs. `specs.md`: a full standard owning a
  spec's shape; global `spec-format.md` § 1 owns whether a spec is needed.
- `docs/standards/` is the roster: every file there binds unless it marks
  itself superseded. `mcp-error-codes.md` is the refusal taxonomy
  (`mcp-errors.md` is superseded). `dependencies.md`: a below-latest pin needs
  a Downgrade Ledger row.
- Do not cite a delta file's section number from memory. Open the file.
- ADRs: `docs/decisions/` (Nygard). Specs: `docs/specs/`. Phase outcomes:
  `docs/journal/`. `docs/plans/` is historical only.

## Versioning & release

- SemVer. **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single
  source of truth.** Never hardcode a version in `.cpp` / `.h`.
- Bump with `cut-release --bump-only`: it touches `CMakeLists.txt`, the
  `Version <strong>X.Y.Z</strong>` banner in `README.md`, and the packaging
  files in `.claude/bump.json`. Re-check README's prose each cycle, and change
  it only where a user-visible claim has drifted.
  `tools/check-readme-claims.sh` runs pre-push.
- CHANGELOG bullets under `[Unreleased]` are written as work lands. The
  version heading is rolled by `new-rc` and dated by `promote`, never by the
  bump.
- Update `PLUGINS.md` in the same commit as any `ants.*` Lua surface change.
- Weekly Wednesday release plus a Patron RC. `-rcN` appears only in the git
  tag, the release title and the AppImage name. Orchestration is
  `packaging/cut-rc.sh` (`new-rc`, `respin`, `promote`, `status`, `cycle`,
  `hotfix`). Flow: `cut-release --bump-only`, then `cut-rc.sh new-rc --push`.
  Wednesday: `cut-rc.sh cycle`. Urgent fix to a published release:
  `cut-rc.sh hotfix <fix-sha>…`.
- `new-rc` builds and tests before it tags: run it backgrounded, never under
  a short timeout. After a killed ninja, run `ninja -C build -n` and
  `-t recompact` first.
- Before `new-rc`, run `bash tools/check-shipped-coverage.sh`. It lists shipped
  items no CHANGELOG bullet cites and bullets that copy a headline, and exits
  non-zero on either, so keep it out of a `set -e` chain. Review each hit.
- **A CHANGELOG entry states what shipped, never the defect.** Do not copy a
  roadmap headline that states a problem. Prefer `changelog_log op:"add"` with
  an authored summary: `add_from_roadmap`, and an id-only `add_batch` entry,
  copy the headline.

## Key design decisions (non-obvious)

- Custom VT100 parser, no pyte/libvterm. Qt6 is the only runtime dep.
- Delayed-wrap (xterm-style) line wrapping.
- Alt-screen 1049 supported.
- Combining chars in a per-line side table.
- Image paste saves the image and inserts its path.
- Lua sandbox strips dangerous globals and has an instruction-count timeout.
- Session persistence via `QDataStream` + `qCompress`.
- `opacity` drives per-pixel terminal-area alpha only; chrome paints opaque.
  No `setWindowOpacity()`.
- Audit: the rule pack is JSON (`audit_rules.json` appends/overrides; hardcoded
  checks stay in C++). `clazy-standalone` for Qt-aware checks.
  `.audit_suppress` is JSONL v2. Calibration reads existing project configs;
  `.audit_allowlist.json` is only for custom grep rules. The audit test
  harness is shell-based against fixture dirs.
- Audit confidence (0–100): floor +10, severity×15, +20 cross-tool, +10
  external AST tool, −5 short grep finding, −20 test path. AI triage caps:
  FALSE_POSITIVE ≤ 30, TRUE_POSITIVE ≥ 80.
- SARIF exports carry `contextRegion` (±3 lines) and `properties.blame`.
  Generated files are skipped.
- Roadmap-query IPC caches parsed bullets with mtime and a 100 ms TTL.

---

Why a rule reads as it does, and the full text before the 2026-09-26 trim:
[`docs/history/claude-md.md`](docs/history/claude-md.md).
