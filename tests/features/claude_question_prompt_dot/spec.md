# AskUserQuestion lights the "awaiting input" tab dot (ANTS-1858)

## Problem

The per-tab Claude state dot only turns orange ("awaiting input") via
`TerminalWidget::checkForClaudePermissionPrompt`, which anchors on the
tool-PERMISSION footer ("Tab to accept" / "Do you want to proceed").
An `AskUserQuestion` prompt blocks Claude on the user just the same, but
renders a different footer ("Enter to select · ↑/↓ to navigate · Esc to
cancel") and fires no `PermissionRequest` hook (it is auto-allowed, not
a gate). So the dot stayed grey (Idle) during a question — the user
couldn't tell from the tab strip that Claude was waiting on them.

**Follow-up (2026-05-25):** the first fix lit the dot but it never
cleared — it stayed orange after the user answered. The clear relied
solely on the footer-gone N=3 debounce, which runs on the trailing-edge
`m_claudeDetectTimer` (single-shot 300 ms, restarted on every PTY
batch). Claude's spinner repaints faster than 300 ms during active work,
so the scan rarely runs and the debounce never accumulates 3 misses; at
idle it fires once. So `claudeQuestionCleared()` never emitted.

Two-layer fix: (a) the detect timer is made a throttle (fires every
~300 ms *during* output, not only after it settles) so the footer-gone
debounce actually accumulates — this is the load-bearing clear and needs
no hooks; (b) a `toolFinished`/`sessionStopped` belt mirrors the
permission path for an instant clear *when* Ants' Claude hooks are
installed. Hook events reach Ants only via an installed
PreToolUse/PostToolUse/Stop hook POSTing to the UDS hook server, which
is not guaranteed — so (a) is what fixes the common case.

## Invariants

### INV-1 — scanner detects the selection-prompt footer

`checkForClaudePermissionPrompt` recognises the AskUserQuestion /
pure-selection footer by the ASCII-stable anchor "Enter to select" and
emits a rule-less `claudeQuestionDetected()` signal when it appears.

### INV-2 — permission prompt wins (no double-fire)

The question branch only fires when no tool-permission footer is
present on screen (`questionFooter && !permFooter`), so a genuine
permission prompt that also renders a select menu still takes the
allowlist-rule path, not the question path.

### INV-3 — debounced clear

When the selection footer leaves the screen, `claudeQuestionCleared()`
fires only after the same N=3 consecutive-miss debounce the permission
footer uses, so a transient TUI repaint doesn't drop the dot mid-prompt.

### INV-4 — mainwindow lights the dot + label, NO button

The `claudeQuestionDetected` handler calls
`markShellAwaitingInput(pid, true)` (orange dot) and
`setPromptActive(true)` ("Claude: prompting" label) for the owning tab,
and creates NO `claudeAllowBtn` / "Add to allowlist" button (a question
has no allowlist rule). `claudeQuestionCleared` calls
`markShellAwaitingInput(pid, false)`.

### INV-5 — tracker awaiting overlay drives the orange glyph

`markShellAwaitingInput(pid, true)` (no rule) sets
`shellState(pid).awaitingInput == true` with an empty `awaitingRule`,
which the glyph provider maps to `Glyph::AwaitingInput` (orange).

### INV-6 — best-effort hook-driven clear belt

For an instant clear *when* Ants' Claude hooks are installed,
`connectTerminal` wires both `ClaudeIntegration::toolFinished` and
`ClaudeIntegration::sessionStopped` to
`TerminalWidget::clearClaudeQuestionPrompt()`, mirroring the permission
path's belt. For a mid-turn `AskUserQuestion` tool call `PostToolUse`
fires on answer and `Stop` at end-of-turn, so neither fires while the
question is on screen — the dot never clears prematurely. This is
best-effort: hook events reach Ants only via an installed hook POSTing
to the UDS server, so the throttled scanner (INV-8) is the load-bearing
clear.

### INV-7 — clear resets the sticky flag (no desync)

`clearClaudeQuestionPrompt()` no-ops unless `m_claudeQuestionActive`,
then resets `m_claudeQuestionActive` + `m_claudeQuestionMissedCount` and
emits `claudeQuestionCleared()`. Resetting the sticky flag is required:
the detect signal only fires on the `!m_claudeQuestionActive` rising
edge, so an external clear that dropped the dot without resetting the
flag would leave the *next* question's dot dark.

### INV-8 — detect timer is a throttle, not a trailing-edge debounce

The PTY-output path starts `m_claudeDetectTimer` only when it is not
already active (`if (!m_claudeDetectTimer.isActive())`), never a bare
`.start()`. A bare start restarts the single-shot timer on every batch,
so during Claude Code's continuous spinner output (frames < the 300 ms
interval) the timer never fires and `checkForClaudePermissionPrompt`
never runs — the footer-gone debounce can't accumulate. The throttle
fires every ~300 ms *during* output, so the question/permission dot
clears within ~3 scans of the footer leaving the screen with no hooks
required. This is the load-bearing clear; INV-6 is the hook fast-path.

## Test plan

INV-1..4, INV-6, INV-7, INV-8 lock the scanner branch + GUI wiring +
clear method + throttle by source-scrape (the scanner needs a PTY-backed
grid; the handler needs a live QStatusBar) — same approach as
`claude_prompt_lifecycle`. INV-5 is exercised behaviourally through the
real `ClaudeTabTracker` API.
