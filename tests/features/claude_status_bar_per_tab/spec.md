# Claude status bar — per-tab session isolation (ANTS-1161)

The bottom-of-window `Claude: <state>` status-bar widget is driven by
`ClaudeIntegration` — a singleton that owns the hook server (one UDS
shared by every Claude Code process under any tab). User report
2026-05-07: focused tab was actively presenting a design ("Claude:
thinking" was correct on the per-tab dot), but the bottom widget read
`Claude: bash` because a sibling tab was running a Bash tool_use and
its `PreToolUse` hook clobbered the singleton's `m_state` /
`m_currentTool`.

The per-tab dot indicator (`ClaudeTabTracker`, ANTS-1118) was correct
because it stores `ShellState` per `shellPid` and derives state
per-shell from each transcript independently. The bottom widget
diverged because it reads the singleton's mutable state directly.

## Invariants

For every hook event arriving on the singleton's UDS:

- **I1 — Foreign session_id is dropped for state mutations.** A hook
  event whose `session_id` does not match the basename of the focused
  tab's transcript (`m_transcriptPath`) MUST NOT mutate `m_state`,
  `m_currentTool`, or emit `stateChanged`, `toolStarted`, `toolFinished`,
  `sessionStarted`, or `sessionStopped`.
- **I2 — Focused session_id is honoured.** A hook event whose
  `session_id` matches the basename of `m_transcriptPath` MUST update
  state and emit `stateChanged` exactly as before the gate was added.
- **I3 — PermissionRequest stays ungated.** Permission prompts route
  per-tab via `m_lastHookSessionId` in the slot, not via the
  singleton's `m_state`. The gate MUST NOT drop these events — they
  belong to whichever tab the prompt is for, and the slot resolves
  the right one downstream.
- **I4 — Pre-poll tolerance.** When `m_transcriptPath` is empty (the
  focused tab's Claude has been bound but `pollClaudeProcess` hasn't
  yet resolved its transcript), the gate MUST default to "accept"
  rather than dropping every event. Without this carve-out the very
  first `SessionStart` would be dropped before `m_activeSessionId`
  could be set.

## Out of scope

- The per-tab dot indicator (`ClaudeTabTracker`) is covered by
  `claude_tab_status_indicator/`. This test does not duplicate that.
- Permission-prompt routing to the right per-tab tracker entry is
  exercised by other call sites of `lastHookSessionId()`. We assert
  only that the gate does not drop the `permissionRequested` signal.
- Real UDS plumbing. The test drives `processHookEventForTest` directly
  — same pattern `claude_status_bar` uses with `parseTranscriptForState`.

## Acceptance

`ctest -L features -R claude_status_bar_per_tab` exits zero. Removing
the gate (the `if (!isFocused) return;` lines in `processHookEvent`)
must cause this test to fail, demonstrating the invariant catches the
cross-tab pollution.
