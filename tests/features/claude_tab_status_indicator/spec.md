# Feature: Per-tab Claude Code activity indicator

## Contract

For each tab whose shell process has a Claude Code child process
running, the tab chrome MUST render a small glyph (or colored dot)
reflecting the current Claude state of **that tab specifically**.
A user with three Claude sessions spread across three tabs MUST be
able to tell at a glance which tab is busy, which is waiting at a
permission prompt, and which is idle — without cycling through them.

## State vocabulary

Mirrors the bottom-status-bar label (see `claude_status_bar/spec.md`)
but adds one state (`AwaitingInput`) that the global label folds into
"Claude: prompting":

| Per-tab state  | Source signal                                                      | Glyph / color                |
|----------------|--------------------------------------------------------------------|------------------------------|
| `NotRunning`   | No Claude process under the tab's shell PID                        | *(no glyph drawn)*           |
| `Idle`         | Last `assistant` event has terminal `stop_reason` (end_turn, …)    | dim / muted dot              |
| `Thinking`     | Last real event is `user` or `assistant` with empty `stop_reason`  | slow-pulse or plain dot      |
| `ToolUse`      | Last `assistant` has `tool_use` content block                      | solid dot                    |
| `Planning`     | Most recent `permission-mode` event has `permissionMode == "plan"` | solid dot (distinct hue)     |
| `Compacting`   | `/compact` in flight                                               | solid dot                    |
| `AwaitingInput`| `PermissionRequest` hook has fired on this tab since last response | **loud, high-contrast dot**, optional pulse; wins over any other state |

`AwaitingInput` is the state the user most needs to act on — so it
overrides whatever transcript-derived state says. Once the user
resolves the prompt (Allow / Deny / Add to allowlist), the glyph
reverts to whatever the transcript parser currently reports.

## Architectural invariants

1. **Per-tab state, not global state.** The `ClaudeIntegration`
   singleton's `m_state` / `m_currentTool` / `m_planMode` describe the
   *active* tab only (by design, per `claude_status_bar/spec.md`
   §"tab-switch clear"). The per-tab indicator MUST NOT read from
   those fields. Instead, it reads from a separate per-tab map keyed
   by tab index (or shell PID).

2. **No bleed across tabs.** State updates on tab A must not modify
   the glyph on tab B. A transcript-watcher fire on tab A's `.jsonl`
   file MUST only update tab A's entry.

3. **Untrack on close.** When a tab closes, its entry MUST be removed
   and its transcript watch released, so a stale glyph doesn't survive
   the tab.

4. **Switching tabs doesn't clear per-tab state.** Unlike
   `setShellPid`, which clears the global active-tab state, tab
   switches MUST leave per-tab entries intact. The glyph on an
   inactive tab remains accurate.

5. **Config gate.** The indicator is controlled by
   `claude_tab_status_indicator` (default on). When disabled, no
   per-tab tracking runs and no glyph renders.

## Scope

### In scope
- Per-tab state detection from each tab's shell PID (proc-walk child
  lookup + transcript tail parse, shared with the existing
  `ClaudeIntegration` logic).
- Glyph rendering on `ColoredTabBar`.
- `AwaitingInput` override on `PermissionRequest` hook (the active
  tab at the time of the prompt wins the flag; resolved when the
  user clicks Allow/Deny/Add-to-allowlist).
- Config toggle.

### Out of scope
- Per-tab context-percent progress bar. Context % stays in the
  bottom status bar for the active tab only — a 12 px tab glyph
  can't carry a percentage readably.
- Per-tab permission button groups. The existing single-permission-
  group UI in the status bar stays (it already clears on tab-switch
  per the whole-status-bar contract). The tab glyph only flags that
  the prompt exists; to act on it the user clicks the tab, then the
  Allow/Deny buttons appear in the usual place.
- Animation of state transitions. `AwaitingInput` MAY pulse; other
  states render static. No cross-state transition animations.

## Invariants enforced by tests

Given the architecture doesn't expose a rendered tab bar without a
full Qt widget harness, the test anchors to the **data path** (the
tracker class) rather than pixel-level painting:

1. **INV-1 Multi-shell mapping.** Instantiating `ClaudeTabTracker`
   and calling `trackTab(0, shellPidA)` + `trackTab(1, shellPidB)`
   with synthetic transcript files for each shell produces two
   independent per-tab snapshots. Mutating one transcript must fire
   `tabStateChanged(tabIndex)` with the right index.

2. **INV-2 State mapping parity.** For each documented transcript
   terminal event (see the state table above), the resulting
   `ClaudeTabTracker::TabState::state` matches the existing
   `ClaudeIntegration::parseTranscriptForState` result. This locks
   the two parsers to the same behavior and prevents drift.

3. **INV-3 Untrack releases resources.** After
   `untrackTab(tabIndex)`, a subsequent write to the old transcript
   MUST NOT fire `tabStateChanged`. The watcher for that path must
   be released (verify via `QFileSystemWatcher::files()` not
   containing the old path OR by asserting no signal emission on
   write).

4. **INV-4 AwaitingInput wins over transcript state.** With a
   synthetic transcript whose tail maps to `ToolUse`, calling
   `markTabAwaitingInput(tabIndex, true)` must cause
   `tabState(tabIndex).state` to read `AwaitingInput`. Calling
   `markTabAwaitingInput(tabIndex, false)` must revert to `ToolUse`
   (the underlying transcript state is retained, not lost).

5. **INV-5 Plan mode latching.** Plan mode follows the same
   latched-across-tail-window rule as `ClaudeIntegration`: if the
   most recent `permission-mode` event in the tail says "plan",
   the per-tab state reads `Planning`; toggling to "default" must
   revert to whatever the transcript parser derives.

6. **INV-6 Session-id → shell-PID routing.** Claude Code writes
   transcripts as `~/.claude/projects/<project>/<session-uuid>.jsonl`.
   `ClaudeTabTracker::shellForSessionId(uuid)` MUST return the tracked
   shell whose stored transcript basename matches the UUID, and 0 for
   empty or unknown inputs. This is what lets the `PermissionRequest`
   hook (which carries `session_id` but not a shell PID) land the
   "awaiting input" flag on the correct tab rather than always on the
   active tab.

7. **INV-7 Bash tool surfaces distinctly.** When the parsed tail has a
   `tool_use` content block with `name == "Bash"`, the per-tab state
   tuple MUST expose `tool == "Bash"` verbatim. The mainwindow
   provider closure keys on that exact string to select the Bash glyph
   variant; a drift (e.g. quoting, lowercasing, prefixing) would
   silently collapse Bash back into the generic ToolUse color.

8. **INV-8 Per-shell transcript path is project-cwd-scoped** *(added
   0.7.48 after a user-visible regression where a multi-Claude-tab
   layout showed the activity dot on only one tab)*. When
   `detectClaudeChild` first attaches a transcript to a shell, the
   path MUST resolve to the Claude project directory matching THAT
   shell's cwd, not the system-wide newest `*.jsonl`. The static
   helper `ClaudeIntegration::sessionPathForCwd(cwd)` is the source
   of truth for the resolution: walk up `cwd`, encode each ancestor
   via `encodeProjectPath` (replace `/` with `-`), probe
   `~/.claude/projects/<encoded>/`, return the newest `.jsonl` from
   the deepest match. With this rule, two shells in two distinct
   project trees end up with two distinct transcript paths, and the
   per-path `m_pathToShell` map fans `QFileSystemWatcher::fileChanged`
   out to the right shell instead of last-write-wins-collapsing N
   shells onto one entry. Tested by:
   - **Source-grep:** `claudetabtracker.cpp` calls
     `ClaudeIntegration::sessionPathForCwd` and reads
     `/proc/<pid>/cwd` rather than walking `claudeProjectsDir()`
     for the system-wide newest in the first-detection branch.
   - **Round-trip:** create two synthetic `~/.claude/projects/`
     subdirs (encoded forms of two different cwds), drop a `.jsonl`
     in each with different mtimes, call
     `ClaudeIntegration::sessionPathForCwd(cwdA)` /
     `(cwdB)`, assert each returns the file from its own subdir.

## Rationale

The existing single-global `ClaudeIntegration` design assumes one
active Claude session at a time. Users running Claude Code in two or
three tabs concurrently (one for each sub-project or agent lane) have
no way to know which tab has pending work without cycling through
them. The bottom status bar can only show one tab's state at a time;
the tab chrome is the natural place to surface the other tabs'
states.

`AwaitingInput` being loud is deliberate: `PermissionRequest` is the
one state where Claude is *blocked* on the user, and missing it
stretches out the interaction. Pulsing / high-contrast treatment is
proportionate to the "you need to act" semantics.

## Regression history

- **2026-04-23 user request.** "For each tab where a Claude Code
  process is running, render a small glyph on the tab chrome
  reflecting the current Claude state — at a glance I want to see
  which tab is busy, which is waiting at a permission prompt, and
  which is idle, and act on the right one without cycling tabs."
  Roadmap entry: `ROADMAP.md` §"🎨 Claude Code UX — per-tab status
  indicator (user request 2026-04-23)".
