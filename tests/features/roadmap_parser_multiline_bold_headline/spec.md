# ANTS-1561 — `roadmap_query` `headline_oneline` for multi-line bold headlines

## Problem

`RoadmapDialog::parseBullets` uses `rxBold = \*\*(.+?)\*\*` (with the
ANTS-1547 lazy quantifier) to capture the bold span at the start of a
bullet body. Without `DotMatchesEverythingOption`, the `.` does not
match `\n`, so a bullet whose bold headline soft-wraps across two
source lines — e.g.

    - 📋 [ANTS-1411] **`cold_eyes_partition` spec-lane detection
      hardcoded to `ANTS-NNNN.md` filename shape.**
      **Layman:** ...

— is silently skipped by the regex. The match falls through to the
*next* `**...**` pair, which is `**Layman:**`. The bullet's
`headline` is then set to `Layman:`, and `headline_oneline` becomes
`"Layman:"` — violating the ANTS-1521 contract that
`headline_oneline` is "newlines + whitespace runs collapsed to a
single space — safe to concatenate".

Observed in-session 2026-05-18 against bullets ANTS-1411/1412/1413/
1414/1419/1420 (every bullet in
`ants-mcp-improvements-from-running-audit-indie-review-debt-sweep-
2026-05-14` whose headline spans two source lines).

## Fix

Add `QRegularExpression::DotMatchesEverythingOption` to `rxBold` in
`src/roadmapdialog.cpp`. The existing lazy `(.+?)` quantifier still
terminates at the *first* `**` after the opening pair, so behaviour
for single-line bold spans is unchanged. `headline_oneline`'s
existing whitespace-collapse pass (`rcHeadlineOneline` in
`src/remotecontrol.cpp:181`) already handles the `\n` inside the
captured text.

## Invariants

- **INV-1.** Single-line bold headline parses as before (no
  regression).
- **INV-2.** Two-line soft-wrapped bold headline captures the full
  span — `headline` carries the joined text (with the `\n` from the
  collected body), `headline_oneline` collapses to one space.
- **INV-3.** Multi-line headline followed by `**Layman:**` does NOT
  capture `Layman:` as the headline (the regression case).
- **INV-4.** Headline length cap (120 chars + `…`) still applies on
  long multi-line headlines.

## Scope

Single regex flag change + four new tests. No CHANGELOG-breaking
shape change to the MCP envelope — pure correctness fix.
