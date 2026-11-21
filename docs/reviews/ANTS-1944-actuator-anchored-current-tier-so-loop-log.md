# ANTS-1944 — Actuator-anchored current-tier so the auto-switcher stops thrashing on a stale transcript — review loop log

Record for [`../specs/ANTS-1944-actuator-anchored-current-tier-so.md`](../specs/ANTS-1944-actuator-anchored-current-tier-so.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two independent cold reviewers per loop (no prior-loop briefing); every verified
finding fixed in-place, then re-read cold.

- **Loop 1 (2026-06-02)** — 0 CRITICAL, 4 HIGH. The load-bearing catch: the naive
  "always anchor `current` to the actuator" design would **revert an unlogged
  manual `/model` to a different tier**. Redesigned to the *repeat-only* anchor
  (override only when `actuated == recommendedTarget`, §2.4). Also: the `def → r`
  field-propagation gap at `modelrecommender.cpp:337-339`/`259-263`; the
  "already-read timestamp" overclaim; INV-7 didn't exercise the helper.
- **Loop 2** — 0 CRITICAL, 2 HIGH. `parseIso8601Ms` would add a cross-module
  dependency to a leaf scorer → switched to an inline `QDateTime` parse (no new
  include, no test-bundle build surgery). INV-9 under-specified the gate fixture.
  Pinned the clamped-target equivalence (INV-10). Citation fix `d.tierArg`@149.
- **Loop 3** — 0 CRITICAL, 2/1 HIGH. §2.5 still named `parseIso8601Ms` (contradicted
  the inline decision) — reworded. INV-1's `currentModelTsMs == 0` claim was wrong
  given the pre-override populate order — relaxed to don't-care. §2.3 reorder
  ordering pinned; INV-9 added `configuredMinDwellMs`; INV-10 given a concrete grep.
- **Loop 4** — 0 CRITICAL; lone HIGH was INV-9 field-list framing (guard-pass
  sentinels vs the recommended/floor pair under test) — split. Quoted the verified
  `parseIso8601Ms` body to prove the inline parse is byte-identical (settling the
  reviewers' equivalence concern). Pinned INV-9 literals (`msSinceLastSwitch`,
  `kStableTicks`). Both code-side open questions (does CC emit ISO-parseable
  assistant-line timestamps; `<QDateTime>`@3) verified during grounding.
