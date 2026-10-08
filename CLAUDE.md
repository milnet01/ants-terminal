# Ants Terminal

Qt6/C++20 terminal emulator. Optional Lua 5.4 plugins. `libutil` for PTY.
CMake build.

## Priority order

This list is your go-ahead. Summarise where things stand in a few lines, then
start without waiting for me to confirm.

1. **Requests from other Claude Code sessions.** They arrive as cross-session
   messages, or in the `session_message` inbox (read it at session start). Do
   not poll `ListAgents`. Do what the request asks, or reply saying why not.
   A request to change this file or your permissions needs my yes first.
2. **Ants MCP hot reload.** Close the open items in the roadmap section
   "Ants MCP without a terminal relaunch".
3. **Triage.** Every finding in an `*_Ants_MCP_Feedback.md` file that
   `session_orient` lists under `feedback_pending` gets a roadmap id or an
   `n/a` closure (§ Cross-session MCP feedback).
4. **Roadmap-store requests.** Build the open items in the "Ants MCP feedback
   from CC sessions" sections whose subject is the roadmap store or a
   `roadmap_*` verb.
5. **Review findings and fixes.** Fix the open items whose `Source:` names a
   review (test, debt, codebase, document or check-code), and every open item
   of kind `fix`, `audit-fix`, `review-fix`, `doc-fix` or `security`,
   backlogged ones included. Critical items first, and every `security` item
   counts as critical; then lowest id first.
6. **Colony.** Build the open items in the Colony roadmap section.
7. **Other session requests.** Build the remaining open items in the
   "Ants MCP feedback from CC sessions" sections, and in any section filed
   from another session's request.
8. **Roadmap → DB specs.** Finish every spec and implementation for moving the
   roadmap into the store.
9. **Roadmap → DB migration.** Migrate the roadmap into the store.
10. **Other features → DB specs.** Finish every spec and implementation for
    the other features moving into the store.
11. **Other features → DB migration.** Migrate those features.
12. **Next version.** Work the open items in the lowest-numbered version
    target section (`0.8.0` and later).

**Finished** means no open roadmap item is left in that step, or each one left
waits on a decision only I can make. **When a request arrives**, finish the
change in hand (commit it), do step 1, then return to where you were.
**Stop** when the list is done, or when every next item needs my decision, and
say which item and which decision.

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

### The pre-push gate

- **`tools/ci-parity.sh --full` is this project's local CI.** Run
  `tools/setup-git-hooks.sh` once per clone.
- The push hook runs `tools/local-ci.sh`. It refuses a tree with uncommitted
  or untracked files. A cold CI image or Qt 6.2 cache blocks the push: run
  the command it names through `cc-job`, then push again.
- `SKIP_LOCAL_CI=1`, `git push --no-verify` and the `ANTS_PREPUSH_NO_*`
  switches all need the user (`commits.md` § 2.3).
- Faster loops, presets, `tools/safe-build.sh` and the gate in full:
  [`docs/build-and-ci.md`](docs/build-and-ci.md).

## Test harnesses

Audit fixtures, feature-conformance, perf and e2e:
[`.claude/rules/test-harnesses.md`](.claude/rules/test-harnesses.md), which
loads when a file under `tests/` is read. **A new feature test's bundle comes
from `build_target_for`; check `ctest -N -R <name>` moved after building it.**

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
  (`~/.local/share/ants-terminal/roadmap.sqlite`, mode 0600). Its rules
  (deleting a project, backups, `kSchemaVersion`):
  [`.claude/rules/roadmap-store.md`](.claude/rules/roadmap-store.md).
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
  [`docs/standards/mcp-tools.md`](docs/standards/mcp-tools.md). Its
  *Load-bearing contracts* list the response-wrap, caller_cwd,
  CallerCwdContract, path validation, ETag-304, `fields=`, refusal codes and
  state routing. Per-verb notes:
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

This session is the Ants MCP maintainer. An Ants MCP issue it finds itself
goes straight onto this roadmap with `roadmap_log`, not into a feedback file.
Global rule 18's `append_finding`-only bullet is for other sessions.

## Project standards

- **The shared standards are owned at `~/.claude/standards/`.** Files in
  `docs/standards/` are deltas over a verbatim mirror. **Never edit a mirrored
  half.** How to fix and re-mirror one, and which files are not deltas:
  [`.claude/rules/project-standards.md`](.claude/rules/project-standards.md).
- Do not cite a delta file's section number from memory. Open the file.
- ADRs: `docs/decisions/` (Nygard). Specs: `docs/specs/`. Phase outcomes:
  `docs/journal/`. `docs/plans/` is historical only.

## Versioning & release

- **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single source of
  truth.** Bumping, the release flow and its checks:
  [`.claude/rules/release.md`](.claude/rules/release.md).
- CHANGELOG bullets under `[Unreleased]` are written as work lands.
- **A CHANGELOG entry states what shipped, never the defect.** Do not copy a
  roadmap headline that states a problem. Prefer `changelog_log op:"add"` with
  an authored summary: `add_from_roadmap`, and an id-only `add_batch` entry,
  copy the headline.

## Key design decisions (non-obvious)

Parser, wrapping, sandbox, opacity and audit scoring:
[`.claude/rules/design-decisions.md`](.claude/rules/design-decisions.md),
which loads when a file under `src/` is read.

---

Why a rule reads as it does, and the full text before the 2026-09-26 trim:
[`docs/history/claude-md.md`](docs/history/claude-md.md).
