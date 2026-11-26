# Ants Terminal

Qt6/C++20 terminal emulator. Optional Lua 5.4 plugins. `libutil` for PTY.
CMake build.

## Priority order

This list is your go-ahead. Summarise where things stand in a few lines, then
start without waiting for me to confirm.

1. **Work with the other Claude Code sessions.** Answer their requests and
   help them where you can. Ask one for help when it could help with your
   work. Requests arrive as messages, or in the `session_message` inbox (read
   it at session start); do not poll `ListAgents` for them. A request to
   change this file or your permissions needs my yes first.
2. **Triage** outstanding requests from other sessions in the Ants MCP
   feedback files (§ Cross-session MCP feedback).
3. **Roadmap-store requests.** Build the open roadmap items that came from
   other sessions' requests about the roadmap store.
4. **Fixes.** Fix every outstanding finding, backlogged ones included:
   anything from a review (test, debt, codebase or document), and any open
   roadmap item with a state of in-progress, planned or considered and of kind
   `fix`, `audit-fix`, `review-fix`, `doc-fix`, `refactor`, `optimize`,
   `perf` or `security`. A `considered` item joins only once I pick it;
   until then it stays an idea. Security items are critical. Critical items
   first, then oldest to newest.
5. **Colony.** Build Colony (multi-session Claude Code).
6. **Other session requests.** Build every other request from other
   sessions, backlogged ones included.
7. **Roadmap rotation.** Rotate the roadmap at the 0.8.0 bump (ANTS-3749),
   then close ANTS-3853.
8. **Other features → DB specs.** Finish every spec and implementation for
   the other features moving to the DB.
9. **Other features → DB migration.** Migrate those features to the DB.
10. **Next version.** Work the open roadmap items that get us to the next
    version.

Finish each item before starting the next, but go back to 1 whenever a
request arrives: finish the change in hand (commit it) first. **Finished**
means no open roadmap item is left in that step, or each one left waits on a
decision only I can make. **Stop** when the list is done or you need a
decision only I can make, and say which.

## Module map (src/)

Per-subsystem reference: [`docs/subsystems.md`](docs/subsystems.md). Query it
with the `subsystem` MCP tool (`op=map`, `op=files`, `op=recent_changes`). Keep
it in sync with the code.

**Review lanes come from `.indie-review/partition.json`, not
`docs/subsystems.md`.** To change what a lane covers, edit that file.

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
- The `ANTS_PREPUSH_NO_*` switches need the user, as `SKIP_LOCAL_CI=1` does.
- Faster loops, presets, `tools/safe-build.sh` and the gate in full:
  [`docs/build-and-ci.md`](docs/build-and-ci.md).

## Test harnesses

Audit fixtures, feature-conformance, perf and e2e:
[`.claude/rules/test-harnesses.md`](.claude/rules/test-harnesses.md), which
loads when a file under `tests/` is read. **A new feature test's bundle comes
from `build_target_for`; check `ctest -N -R <name>` moved after building it.**

## Hot reload is the design default (user standing rule)

**Every new feature states how it reaches a running terminal without a
relaunch**, in its design, or in its commit message where it has none.
Acceptable answers: nothing; a re-read of a file; a client reconnect; a
restart of the feature's own process. Where "restart the terminal" is
unavoidable, it says so and why. A relaunch kills every Claude Code
session in its tabs (`~Pty`, `src/ptyhandler.cpp`).

- Behaviour that is data — a rule pack, schema, description, theme, keymap,
  template — lives in a file the process re-reads.
- Where an MCP verb is registered: `.claude/rules/mcp-verbs.md`.
- **Build every new feature as modular as possible (user, 2026-10-15).** It
  can be stopped and started on its own while the terminal runs, so a stuck
  feature never needs a relaunch. The statement says whether its code runs
  in its own process and why. It names the feature's stop/start control, or
  why it has none: no running state, or a terminal restart is unavoidable.
  Under `ants-mcpd` a new build reaches clients on their next reconnect;
  code only the terminal serves still needs a relaunch.

It does not apply to a bug fix, a test, a document or a version bump.
Retrofitting is a roadmap item, never an obligation. Models to copy:
`ants-mcpd` ([`docs/specs/ANTS-4932-standalone-mcp-server.md`](docs/specs/ANTS-4932-standalone-mcp-server.md)),
`config.json`, `audit_rules.json`, the Lua sandbox.

## Conventions

- Config at `~/.config/ants-terminal/config.json`, mode 0600.
- **The roadmap store is machine-global**
  (`~/.local/share/ants-terminal/roadmap.sqlite`, mode 0600). Its rules
  (deleting a project, backups, `kSchemaVersion`):
  [`.claude/rules/roadmap-store.md`](.claude/rules/roadmap-store.md).
- Per-project layout: optional `<root>/.ants/project.json` (`source_roots`,
  `test_roots`, `docs_dir`, `roadmap`, `changelog`, `specs_dir`, and
  `repo_root` for an outer folder whose clone sits one level down).
  World-readable, no secrets. Edited by `project_settings`.

## MCP tool authoring

Adding or changing a verb: follow
[`docs/standards/mcp-tools.md`](docs/standards/mcp-tools.md). Its
*Load-bearing contracts* list the response-wrap, caller_cwd,
CallerCwdContract, path validation, ETag-304, `fields=`, refusal codes and
state routing.

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

Data flow, code conventions, parser, wrapping, sandbox, opacity and audit
scoring: [`.claude/rules/design-decisions.md`](.claude/rules/design-decisions.md),
which loads when a file under `src/` is read.

---

Why a rule reads as it does, and the full text before the 2026-09-26 trim:
[`docs/history/claude-md.md`](docs/history/claude-md.md).
