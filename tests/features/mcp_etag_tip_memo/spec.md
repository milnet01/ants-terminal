# ANTS-1568 — Etag-supporting tools' descriptions carry a one-line "Etag tip"

## Problem

ANTS-1499 added the etag short-circuit ("304 Not Modified" pattern)
to eight read tools — `project_layout`, `roadmap_query`,
`file_outline`, `last_audit_summary`, `get_environment`, `tab_list`,
`subsystem`, `git_state`. The `etag_match` parameter doc explains
WHAT the field does ("If the server's current etag equals this
value, the response is short-circuited…"), but no tool description
suggests WHEN a caller should use it.

MAME Curator 2026-05-18 (late evening): "saw the parameters but
didn't use them — no how-to memo at the description level."

## Fix

In `processTools`'s tools/list post-processing loop (same site as
ANTS-1518's `[<kind>] ` prefix injection), append a one-line "Etag
tip" memo to every tool description for which
`isEtagSupportedTool(name)` returns true. Idempotent sentinel:
check for the literal `"Etag tip:"` substring before appending so
a hot-reload doesn't double-append.

## Invariants

- **INV-1.** The eight etag-supporting tools each have a description
  containing the literal `Etag tip:` after the ANTS-1518 prefix
  injection runs. Source-scrape on `claudeintegration.cpp`
  confirms the conditional append at the prefix-injection site.
- **INV-2.** Non-etag-supporting tools do NOT have the memo. The
  conditional `isEtagSupportedTool(name)` is the gate; the memo
  is meaningless on non-etag tools and would be noise.
- **INV-3.** Idempotent. The literal `Etag tip:` substring is the
  sentinel; appending twice would emit `... Etag tip: ... Etag
  tip: ...`. Source-scrape asserts the guard is present.

## Scope

Single block edit in `processTools`'s description post-processing
loop. No descriptor shape change; the memo is appended to the
existing `description` string. Saves the cross-session
discoverability lookup on every etag-supporting tool without
touching the eight separate descriptor blocks.
