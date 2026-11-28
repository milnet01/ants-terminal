# Feature: terminal verbs refuse a number that is not a whole int (ANTS-6989)

`QJsonValue::toInt()` returns 0 for `1.5` or `1e10`. A verb that read `tab`
that way acted on tab 0; one that read `lines`, `max_bytes` or `max_results`
that way fell back to its default as if the argument were absent. Each verb
below checks its numbers with `RemoteControl::wholeIntArg` before it reads a
tab, as `send-text` does (ANTS-6637).

## Invariants

**INV-1 — get-text, recent_errors and last_selection refuse `bad_args`.**
A `tab`, `lines` or `max_bytes` (get-text), `tab`, `lines` or `max_results`
(recent_errors), or `tab` or `max_bytes` (last_selection) of `1.5` or `1e10`
refuses `bad_args`. *Test:* `RcWholeIntArgs.Inv1FractionOrHugeNumberRefusesBadArgs`,
driven on a bare `RemoteControl`.

**INV-2 — select-window and set-title refuse a `tab` that is not whole.**
Both read `tab` through `wholeIntArg`. *Test:*
`RcWholeIntArgs.Inv2SelectWindowAndSetTitleCheckTheTab`, a scrape of the two
bodies: both handlers are private.

## Regression history

- **ANTS-6989:** found by the ANTS-6637 helpers, which fixed `send-text`
  alone. Locked by this spec.
