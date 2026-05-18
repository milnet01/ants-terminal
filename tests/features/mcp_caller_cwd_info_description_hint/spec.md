# ANTS-1578 — `caller_cwd_info` description carries "Use FIRST when…" hint

## Problem

RetroArch Bundle 63 (2026-05-17): caller discovered `caller_cwd_info`
exists only because ToolSearch ranked it. The description ends with
"No side effects — does not read scrollback, run git, or write any
state" — structurally important but doesn't suggest *when* to use
it. The verb is the diagnostic-first stop when a project-scoped
read returns `no_roadmap_loaded` or `cwd_mismatch`, but the user
had to discover that by trial.

ANTS-1418's `selection_hint` field already names the trigger, but
that field is a separate property; many tools/list consumers only
surface the main description.

## Fix

Add a "Use this FIRST when a project-scoped read returns
`no_roadmap_loaded` or any tool returns `cwd_mismatch`" sentence to
the main `description` field of `caller_cwd_info`, mirroring the
existing `selection_hint`. Catches consumers that only read the
description text.

## Invariants

- **INV-1.** `caller_cwd_info`'s `description` literal in
  `src/claudeintegration.cpp` contains the substring
  `Use this FIRST when` AND `no_roadmap_loaded` AND `cwd_mismatch`.
- **INV-2.** The pre-existing `selection_hint` is preserved (the
  description hint is a parallel surface, not a replacement).

## Scope

Single string-literal edit. No behavioural change; descriptor shape
unchanged.
