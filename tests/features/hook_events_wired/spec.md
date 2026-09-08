# Feature: the hook events installed and the hook events handled agree

## Problem

Two lists decide whether a Claude Code hook ever reaches Ants:

- `claudeHookEvents()` in `src/settingsdialog.cpp` — the single source
  of truth ANTS-2205 introduced. `installClaudeHooks` iterates it to
  write entries into `~/.claude/settings.json`, and
  `refreshClaudeHooksStatus` iterates it to decide whether to show
  "Hooks installed".
- the `hookName == "..."` chain in
  `ClaudeIntegration::processHookEvent` — what Ants does with an event
  once it arrives.

Nothing checked that they agree, and they did not.
`processHookEvent` handles `PermissionRequest` and
`PostToolUseFailure`; the installer wired neither. Both are real Claude
Code events — the official hooks reference documents `PermissionRequest`
as "When a tool call needs a permission decision" and
`PostToolUseFailure` as "After a tool call fails" — so these were not
dead branches but working code that never received input.

Both have live consumers. `PermissionRequest` emits
`permissionRequested`, `PostToolUseFailure` emits `toolFinished(tool,
false)`, and both are consumed by the Claude status widgets.
`PermissionRequest` even carries a hardening pass of its own
(ANTS-2190, its cold-start exemption removed). All of that was
unreachable.

A missing entry is silent in both directions. Nothing logs an event
that never arrives, and the hooks status stays green because it
verifies only the list the installer writes.

## Contract

The set of event names in `claudeHookEvents()` and the set of event
names compared in `processHookEvent` MUST be equal.

Equality rather than containment, in both directions:

- A handled event the installer does not wire is a feature that never
  runs — the defect above.
- An installed event the handler ignores spawns the forwarder script,
  a Python interpreter and a socket connection every time it fires, to
  reach a branch that does nothing.

## Invariants

**INV-1 — every event `processHookEvent` handles is installed.**

**INV-2 — every event the installer writes is handled.**

**INV-3 — both lists are non-empty.** A parse that silently found
nothing would satisfy INV-1 and INV-2 vacuously; this is the guard
against the test passing because it stopped looking.

**INV-4 — `PermissionRequest` and `PostToolUseFailure` are among
them.** Named because they are the two the sweep found missing, so a
revert is caught by name rather than only by the set comparison.

## Scope

### In scope
- Source-grep over `src/settingsdialog.cpp` and
  `src/claudeintegration.cpp`.

### Out of scope
- Whether every event Claude Code offers should be wired. The
  reference documents many more; wiring one costs a process spawn per
  occurrence, and this contract is about the two lists agreeing, not
  about coverage of the upstream API.
- What each handler branch does with its event. Owned by the specs for
  those behaviours.
- Migrating an existing `settings.json`. The installer already merges
  rather than overwrites, and preserves a user's own hooks on the same
  event; a user on an older install sees the hooks status stop
  claiming "installed" until they re-run it, which is what ANTS-2205
  built that check to do.

## Regression history

- **ANTS-2205:** made `claudeHookEvents()` the single source of truth
  so the installer and the status check could not drift from each
  other. It did not relate either to the handler.
- **ANTS-4457 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "`PermissionRequest` has no producer: the installer wires five
  events and this is not among them, yet it has a header contract, a
  live consumer in the status widgets and a hardening pass (ANTS-2190).
  `PostToolUseFailure` claimed to be the same shape." Verified against
  source — both branches present, neither installed, and the live
  `~/.claude/settings.json` on the development host confirmed it —
  and fixed. Locked by this spec.
