# `roadmap_log op:append` stable-prefix hint — feature-conformance test

Locks the invariants in `docs/specs/ANTS-1877.md`. The
`counter_read_failed` envelope splits into three codes depending
on the file-state + bullet-shape detected.

## Anchors

| INV | Test                                  | What it checks |
|-----|---------------------------------------|----------------|
| 1   | `Inv1FileMissingVsUnreadable`         | Both new codes (`counter_missing`, `stable_prefix_unsupported`) appear; `counter_read_failed` retained. |
| 2   | `Inv2SnifferContract`                 | `rlDetectStablePrefixId` helper exists with the documented signature. |
| 3   | `Inv3StablePrefixEnvelope`            | Envelope carries `detected_prefix_example` + `follow_up`. |
| 4   | `Inv4CounterMissingHint`              | `echo 0 >` recipe hint literal present. |

## Pre-fix verification

Before the fix, the source contains `counter_read_failed` once,
and the literals `stable_prefix_unsupported` / `counter_missing` /
`detected_prefix_example` / `follow_up` / `rlDetectStablePrefixId`
are absent. After the fix, every test turns GREEN.
