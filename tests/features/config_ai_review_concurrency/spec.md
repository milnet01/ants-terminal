# Feature: ai_review_concurrency config key

## Problem

ANTS-1727 § 2.6 adds a config key controlling the review-dialog
`LlmDispatcher`'s bounded concurrency. It must default to 2 and clamp to
`[1, 4]` so a large partition can't open more sockets or exceed the
~40 MiB transient RAM budget (spec § 4).

## Invariant under test

- **INV-14** — `Config::aiReviewConcurrency()` returns 2 when unset and
  clamps stored/written values to `[1, 4]` (both getter and setter clamp).

## Test notes

Behavioural: constructs a sandboxed `Config` under a temp
`XDG_CONFIG_HOME`, drives the setter across the boundary, reads back.
Label `features;fast`.
