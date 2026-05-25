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
- `-DANTS_UNITY_BUILD=ON` — experimental, opt-in; the current STATIC-lib
  layout breaks test-bundle links (ANTS-1553 tracks the rework).
- `cmake --preset=fast` — ccache + per-lib PCH in `build-fast/`; best for
  hot loops (Unity off until ANTS-1553).

### CMake presets

Each preset: `cmake --preset=X && cmake --build --preset=X && ctest --preset=X`.

| Preset | Use |
|---|---|
| `default` | Release + Ninja in `build/`; honours the JOB_POOLS cap. |
| `workstation` | Release in `build-workstation/`, hard-capped `-j3` for constrained hardware / heavy desktop sessions. |
| `debug` | Debug + ASan/UBSan in `build-asan/`, sanitizer env wired into `ctest --preset=debug`. |
| `fast` | Release in `build-fast/` with ccache + per-lib PCH (Unity off). |

### Backstop: `tools/safe-build.sh`

Wraps `cmake --build` in a systemd-user scope (`MemoryMax=24G` /
`MemorySwapMax=8G`) so a future over-parallelism regression kills the
*build*, not the session. Layers 1–2 (JOB_POOLS, `workstation` preset)
should make it unnecessary; reach for it after kernel / Qt-major updates.
Optional audit deps self-disable if absent. **Cppcheck gotcha:** pass
`--library=qt` or it misparses `emit` as a type.

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
- Scrollback default 50k, max 1M.
- Theme colors set on `TerminalGrid`; ANSI palette (16+216+24) lives there.
- QTextLayout for ligature shaping.

## MCP tool authoring

When adding or modifying an MCP tool, follow
[`docs/standards/mcp-tools.md`](docs/standards/mcp-tools.md) (the umbrella
checklist). The load-bearing contracts, each with its spec:

- **Response wrap (ANTS-1294).** `tools/call` replies are auto-wrapped in
  `<ants_mcp_data tool="…">…</ants_mcp_data>` by
  `ClaudeIntegration::wrapMcpData`. Register normally and the dispatch
  site wraps; control-plane tools (`get_session_info`, `token_usage`,
  `tool_info`) bypass.
- **caller_cwd resolution (ANTS-1401).** Consume `caller_cwd` via
  `ants::resolveCallerCwdRoot` (`src/resolvedroot.h`) — never
  re-implement canonicalisation / tab-walks.
- **CallerCwdContract (ANTS-1404).** Classify each tool at
  `callerCwdContractFor` as Required / Optional / TabSpecific /
  ProcessGlobal; `Required` refuses empty `caller_cwd` with
  `code:"caller_cwd_required"`. Unclassified defaults to Optional.
- **Path validation (ANTS-1295).** Any path-typed arg routes through
  `PathValidation::validatePath` (`src/pathvalidation.h`) before any FS
  op; reject `code:"bad_path"`. Use `check.argvForm` for argv,
  `check.resolved` for the canonical path (empty if not-yet-existing).
- **ETag 304 (ANTS-1499).** Read tools opt in via `isEtagSupportedTool` +
  `makeEtagMatchProp()`; a matching `etag_match` short-circuits to
  `{ok, unchanged, etag}`.
- **`fields=` projection (ANTS-1720).** Opt in via `isFieldProjectionTool`
  + `makeFieldsProp()`; narrows to named top-level fields (a subset of the
  ETag set — list `"etag"` in `fields` to keep 304).
- **Refusal codes** follow
  [`docs/standards/mcp-error-codes.md`](docs/standards/mcp-error-codes.md);
  **caches** follow [`docs/standards/mcp-caches.md`](docs/standards/mcp-caches.md)
  (a path-keyed cache may go cold but must never *shadow*).
- **State routing (ANTS-1336 / ANTS-1435).** `session_memory` /
  `workflow_state` *writes* go through RcGate (focused-tab match);
  *reads* anchor to `caller_cwd`. `wf.<skill>` keys purge at 72 h;
  `session_memory` has no TTL. Storage
  `~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json`. See ANTS-1435
  §Limitations.

Behavioral notes (not authoring rules): `get_scrollback` since-cursor
incremental mode (ANTS-1500); `roadmap_query` recognises ants-v1 /
github-task-list / pass-headings formats (ANTS-1530); `read_log` filters
a log file (Ants debug log or a `caller_cwd` path) to matching lines via
the pure `ReadLog::filter` helper, streaming drop-oldest byte cap +
since_cursor incremental tailing (ANTS-1855).

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
`packaging/cut-rc.sh` (`new-rc` / `respin` / `promote` / `status`), NOT
the global `/release` skill. Flow: `/bump` to base `X.Y.Z`, then
`cut-rc.sh new-rc --push`. `release.yml` routes RC AppImages to a separate
zsync channel so stable users can't auto-update onto an RC.

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
