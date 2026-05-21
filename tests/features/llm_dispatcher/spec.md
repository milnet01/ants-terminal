# Feature: LlmDispatcher — bounded-concurrency job pool

## Problem

The review-dialog family (ANTS-1721/1722) dispatches a set of per-lane /
per-chunk briefs to the configured LLM endpoint in parallel, but bounded
so a large partition can't open dozens of sockets or blow the RAM budget.
ANTS-1727 § 2.2 adds `LlmDispatcher` — a bounded pool over `LlmClient`
with a `JobRunner` test seam so the scheduling logic is deterministically
testable without a network.

## Invariants under test (ANTS-1727)

- **INV-6** — never more than `maxConcurrent` jobs in flight.
- **INV-7** — every enqueued job runs exactly once; `allFinished` fires
  exactly once after the last completes.
- **INV-8** — the dispatcher retains no `result.text` after emitting
  `jobFinished` (result forwarded by const-ref, never stored).
- **INV-9** — `cancelAll` clears the queue and emits `allFinished` once
  the in-flight set drains; no `jobFinished` for cancelled jobs.
- **INV-14 (clamp)** — `maxConcurrent` is clamped to `[1, 4]`.

## Test notes

Injected fake runner stores completion callbacks; the test fires them in
FIFO order to drive scheduling deterministically (no network, no event
loop — direct signal connections). Label `features;fast`.
