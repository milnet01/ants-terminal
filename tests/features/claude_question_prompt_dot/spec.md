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

## Test plan

INV-1..4 lock the scanner branch + GUI wiring by source-scrape (the
scanner needs a PTY-backed grid; the handler needs a live QStatusBar) —
same approach as `claude_prompt_lifecycle`. INV-5 is exercised
behaviourally through the real `ClaudeTabTracker` API.
