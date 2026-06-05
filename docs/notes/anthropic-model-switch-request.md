# Feature request for Anthropic — programmatic model switch for a running Claude Code session

**Status:** FILED 2026-06-05 →
<https://github.com/anthropics/claude-code/issues/65586> (ANTS-1979 parked
pending this; related prior art: #24947, #53049).

## Summary

Expose a **control-plane way for an external tool to change the active model
of an already-running, interactive Claude Code session** — without simulating
keystrokes into the PTY.

## Problem / motivation

Ants Terminal hosts `claude` sessions and would like to auto-select the model
tier per workload (e.g. drop to a cheaper tier during long mechanical
stretches, return to Opus when the work turns hard). Today the **only**
mechanism available to an external host is to inject `/model <tier>\r`
keystrokes into the terminal PTY. That is fundamentally unsafe:

- Injecting while a tool/command is running **cancels the in-flight command**
  (the keystrokes land in the running process / interrupt the turn).
- Injecting between turns risks the confirm dialog + any continuation
  **starting unrequested billable work** — a real cost concern on metered
  plans.
- It depends on scraping the TUI's "Switch model?" dialog strings, which
  change between releases and require perpetual upkeep.

Confirmed there is no clean alternative today: hooks receive the model
read-only; `settings.json` / `ANTHROPIC_MODEL` apply only at startup, not to a
live session; the Agent SDK is one-shot (non-interactive); remote-control /
channels can't invoke slash commands. Keystroke injection is the only path.

## Proposed mechanisms (any one would suffice)

1. **IPC / control socket command** — a documented local control channel for a
   running session accepting a `set-model <tier>` command (and ideally other
   slash-command equivalents), analogous to a "headless control" surface.
2. **A hook that can change the model** — e.g. a `UserPromptSubmit` (or a new
   `TurnStart`) hook whose JSON output may set the model for the upcoming turn.
3. **A watched setting** — a session-scoped file/env the CLI re-reads each turn
   so an external tool can change the tier between turns.

## Key requirement

The switch must be applicable **at a safe boundary (between turns)** and must
**never start new work on its own** — changing the model should not imply
"continue". An external tool needs to set the tier and have it take effect on
the *next user-initiated turn*, with no side effects if the session is idle.

## Why it matters

A clean API turns a fragile, billing-risky, TUI-scraping integration into a
robust one — enabling terminal hosts and automation tools to do cost-aware
model selection safely.
