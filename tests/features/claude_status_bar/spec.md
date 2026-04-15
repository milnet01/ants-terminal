# Feature: Claude Code status-bar reliability

## Contract

The status-bar label showing Claude Code's current state MUST:

1. **Reflect the correct state for a given transcript.** Given a
   `.jsonl` transcript that terminates with a specific event type,
   the derived `ClaudeState` must be:

   | Last meaningful event | Derived state |
   |---|---|
   | `assistant` with `tool_use` content block | `ToolUse`, with `m_currentTool` = tool name |
   | `assistant` with `stop_reason=end_turn` / `max_tokens` / `stop_sequence` / `refusal` | `Idle` |
   | `assistant` with empty/null stop_reason (still streaming) | `Thinking` |
   | `user` carrying a `tool_result` | `Thinking` (processing result) |
   | `user` carrying a plain prompt | `Thinking` |
   | (no events) | *unchanged* |

   Metadata-only events (`system`, `last-prompt`, `permission-mode`,
   `file-history-snapshot`, `summary`, `meta`) MUST be skipped when
   walking backward — they're always the tail of the file but never
   determine the turn's state.

2. **Clear state immediately on shell-PID change** (tab switch).
   When `setShellPid(newPid)` is called with a PID different from
   the current one:
   - `m_state` must be set to `NotRunning`
   - `m_currentTool` must be empty
   - `stateChanged(NotRunning, …)` must be emitted
   Before the next poll tick fires, so the UI reflects the tab switch
   within one event-loop iteration.
   (This ensures tab A's "Claude: thinking..." doesn't bleed into
   tab B, which is the user-reported "doesn't work half the time"
   symptom.)

3. **Update context percent from usage data.** When any `assistant`
   event in the transcript window carries `message.usage.input_tokens
   > 0`, emit `contextUpdated(percent)` where `percent = min(100,
   input_tokens * 100 / 200_000)`.

## Rationale

- Claude Code is the project's flagship integration and the #1 source
  of user-visible status in the status bar.
- The current single-global-`ClaudeIntegration` design couples one
  watcher to whichever tab is focused, but the cached state persists
  across `setShellPid` calls until the next poll (~1s later). Users
  see stale "thinking..." after switching tabs.
- The transcript parser is the feature's core state machine; a bug
  there (metadata event mis-filter, wrong stop_reason handling, etc.)
  silently breaks status across the board.
- Adding automated coverage for both halves (parser + tab-switch)
  keeps the feature honest as Claude Code's event schema evolves.

## Scope

### In scope
- Transcript → state mapping for every documented turn-terminating
  event type.
- Metadata event filtering.
- Tab-switch state invalidation via `setShellPid`.
- Context-percent derivation from `usage.input_tokens`.

### Out of scope
- The UI label itself (`QLabel` render + colour themes) — that's
  straightforward widget code and doesn't have user-visible
  unreliability.
- The hook server + MCP server paths — separate integrations with
  their own contracts.
- Multi-tab concurrent Claude sessions (two Claude processes running
  in two tabs simultaneously). Out of scope until the architecture
  supports per-tab `ClaudeIntegration` instances; the current single-
  global design sees one tab at a time.
- Per-tool routing (the status bar shows the tool name but not
  per-tool specialised UI); tested elsewhere if needed.

## Regression history

- **0.6.22 user report:** "The Claude status indicator doesn't work
  half the time." Diagnosis: on tab-switch, `ClaudeIntegration`
  re-points `m_shellPid` but keeps the stale `m_state` /
  `m_currentTool` / `m_contextPercent` until the next poll cycle
  (default 1 s). Fix pending in CS3; this spec + test lock the
  contract so the fix is verifiable.
