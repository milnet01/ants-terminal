# ANTS-1120 — Companion-instrumentation measurement (placeholder)

**Status:** Not yet run. This file is the placeholder per
`docs/specs/ANTS-1120.md` § Deliverables; it gets populated in-place
when the measurement run actually executes.

## Why this file exists empty

Running the measurement burns Claude API tokens and requires a
scripted task plus stub captures. v1 of ANTS-1120 lands the
infrastructure (this placeholder + `scripts/measure-companion-tokens.sh`
+ `scripts/measure-companion-stubs/`) so the actual run is a single
script invocation when the user is ready, not a "build the harness
first" lift.

## What lands here when the run executes

Per `docs/specs/ANTS-1120.md` § Deliverables:

1. **Per-prompt, per-run token counts** for both Configuration A
   (baseline — Claude reads stdout directly) and Configuration B
   (instrumented — stubbed `audit-run` / `roadmap-query` /
   `id-allocate`). N ≥ 3 replays per configuration (INV-2).
2. **Composite token totals** per run, mean + standard deviation
   across the N runs per configuration (INV-3).
3. **Measured ratio** (`baseline_mean / instrumented_mean`) and a
   **conservative ratio** (1σ pessimistic on baseline, 1σ optimistic
   on instrumented) (INV-4).
4. **Per-downstream-bullet verdict** (keep / iterate / drop /
   inconclusive) chosen by the user *after* reading the
   measurement, with one-line justifications. The user picks the
   keep/iterate/drop thresholds at this point (INV-5).
5. **ROADMAP edits** reflecting the verdicts back into the
   long-form bullets (ANTS-1110 / 1111 / 1112 / 1113 / 1114 / 1116
   v2 / 1117 v2). "Drop" gets a `**Retired:**` annotation; "iterate"
   gets a `Source: measurement-ANTS-1120-<date>` line on the trimmed
   body (INV-6).

## Pre-requisites before running

- A representative scripted Claude task that exercises at least
  three of the instrumented surfaces (audit, roadmap query, ID
  allocation). The task script shape is TBD — pick the workload
  before scheduling the measurement.
- One stub capture per surface in
  `scripts/measure-companion-stubs/`, taken from production
  tooling output against the current tree at the time of capture.
- A working `claude` CLI with API key in env.

## Cross-refs

- Spec: [`docs/specs/ANTS-1120.md`](../specs/ANTS-1120.md)
- ADR: [ADR-0002 § Decision 5](../decisions/0002-cold-eyes-companion-cleanup.md)
- Harness: [`scripts/measure-companion-tokens.sh`](../../scripts/measure-companion-tokens.sh)
