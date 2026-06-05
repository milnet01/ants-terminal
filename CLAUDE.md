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
- `-DANTS_UNITY_BUILD=ON` — experimental, opt-in; the current STATIC-lib
  layout breaks test-bundle links (ANTS-1553 tracks the rework).
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
since_cursor incremental tailing (ANTS-1855); `feedback_query` /
`feedback_log` read/write the `*_Ants_MCP_Feedback.md` files via the pure
`FeedbackFile` module (delta parse + block render; ANTS-1961/1962),
suffix-guarded on `_Ants_MCP_Feedback.md`, append-only at EOF; `spec_log`
writes a spec's Status line / cold-eyes loop log / `INV-N` via the pure
`SpecLog` module (`op:"set_status"` / `"append_loop"` / `"append_inv"`,
never renumbering; reuses `spec_query`'s id routing; ANTS-1963);
`model_switch_stats`
(ANTS-1735, extended by ANTS-1889, sharpened by ANTS-1891) — Required
`caller_cwd`, ETag + `fields` opt-in, aggregates the model-switch
ledger into avoided/regret ratios and pending-record counts (the trust
signal that gates §8 OQ-3 default-ON flip; never writes ledger/config).
Envelope surfaces the live switcher config (`auto_model_switch_enabled`,
`floor_tier`, `min_dwell_sec`) + `scope` echo so callers can tell
"feature OFF" from "ON, no candidates yet" from "ON with measured
outcomes"; accepts optional `scope:"project"` (default) or `"global"`
arg to aggregate across all projects (ANTS-1889). ANTS-1891 —
`regret_count` includes under-route harm; `measured_downgrades`
excludes inconclusive 0-turn records (counted in new
`inconclusive_count` instead); `clean_end_count` + `weighted_avoided`
credit clean session-ends (no override / correction / under-route
within ~10 min of session end) as ½ Opus turn avoided each so end-of-
task downgrades — the dominant ledger shape — are no longer invisible;
headline withholds the ratio until a configurable floor of measured
downgrades is reached and reads "calibrating (N/F measured)" below
the floor (ANTS-1909 renamed the pre-floor phrase from "insufficient
data"). Envelope readers should check `measured_downgrades > 0` before
treating `regret_rate` as meaningful (the new `near_misses` block —
ANTS-1894 INV-12 — is independent and meaningful from the first
record). ANTS-1894 — envelope additionally carries a slim
`near_misses:{total_24h, dominant_blocker}` block; pass
`mode:"near_misses"` for the full blocker breakdown. ANTS-1909 — the
headline now also carries the `dwell=Ns` parenthetical and, when the
24 h near-miss block is non-empty, appends "N near-misses in 24 h
blocked by <dominant_blocker>" on both the no-switches and calibrating
branches so the trust signal reads as "evaluating but blocked" rather
than "feature did nothing". ANTS-1944 — `g.current` is actuator-anchored (repeat-suppression only): a
pure `reconcileCurrentTier` helper in `modelautoswitch` overrides the stale
transcript read with the actuator's last-injected tier, but ONLY when the
clamped recommendation equals that tier (suppressing a re-fire, never
reverting a user's manual `/model`). Provenance fields on `Result`
(`currentModelFromCommand`, `currentModelTsMs`) let the helper distinguish
a fresh command read (always wins) from an assistant-turn read that may be
stale. ANTS-1941 — the trust signal now counts only
current-epoch records: `StatsConfig::minEpoch` is set to
`kSwitcherEpoch` at both dispatch sites so pre-fix contamination is
filtered out by the epoch boundary, not the age window (the age window
can't evict recent pre-fix records). Envelope adds `min_epoch` +
`excluded_pre_epoch_count` when epoch-filtered; callers reading
`regret_rate` from the live MCP verb now measure only the current
switcher behaviour. `statsForProject` / `statsEnvelope` and any
`minEpoch=0` caller remain unaffected (all-time forensic view).

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

## Cross-session MCP feedback

Other CC sessions (Vestige, MAME Curator, Album Builder, RetroArch,
RetroDB) write MCP observations to `*_Ants_MCP_Feedback.md` files under
`/mnt/Games/Scripts/Linux/`. Format spec:
[`docs/standards/mcp-feedback-files.md`](docs/standards/mcp-feedback-files.md).

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
