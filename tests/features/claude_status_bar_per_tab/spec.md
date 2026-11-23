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
- **I3 — PermissionRequest stays ungated on the warm path.** Once
  `m_transcriptPath` is resolved, permission prompts route per-tab via
  `m_lastHookSessionId` in the slot, not via the singleton's
  `m_state`. The gate MUST NOT drop these events — they belong to
  whichever tab the prompt is for, and the slot resolves the right one
  downstream. (Cold-start is the exception — see I5.)
- **I4 — Pre-poll tolerance.** When `m_transcriptPath` is empty (the
  focused tab's Claude has been bound but `pollClaudeProcess` hasn't
  yet resolved its transcript), the gate MUST default to "accept"
  rather than dropping every event. Without this carve-out the very
  first `SessionStart` would be dropped before `m_activeSessionId`
  could be set.
- **I5 — Cold-start PermissionRequest is dropped (ANTS-2190).** During
  cold-start (the poll has found the focused tab's Claude, `m_claudePid`
  non-zero, but `m_transcriptPath` is still empty) `isFocusedTabSession()`
  returns true for ANY `session_id`, so a sibling tab's `PermissionRequest`
  cannot be attributed to the focused tab. It MUST be dropped — neither
  `permissionRequested` emitted nor `m_lastHookSessionId` committed —
  rather than self-route via a poisoned last-seen session and
  mis-attribute the prompt to the focused tab. Only `SessionStart`
  still falls through cold-start (it bootstraps `m_activeSessionId`,
  and is itself refused from committing the session id, ANTS-1996). The
  warm-path I3 behaviour is unchanged.
- **I6 — With no Claude in the focused tab, PermissionRequest fires
  (ANTS-6533).** When `m_claudePid` is zero the transcript path stays
  empty, which is not cold-start. A `PermissionRequest` from another
  tab's Claude MUST emit `permissionRequested` and commit
  `m_lastHookSessionId`, so the slot routes it to that tab. State
  hooks are still dropped.
- **I7 — A question is not a permission (ANTS-6898).** Claude Code sends
  `PermissionRequest` for `AskUserQuestion`. That event MUST NOT emit
  `permissionRequested`, so no Allow / Deny / Add to allowlist bar
  appears; the question path (ANTS-1858) shows the tab as awaiting
  input. Test: `ClaudeStatusBarPerTab.QuestionIsNotAPermission`.

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
