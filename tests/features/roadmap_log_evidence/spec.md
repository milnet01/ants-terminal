# roadmap_log evidence:[paths] — Feature Spec (ANTS-3382)

## Purpose
A bug logged from screenshots / logs should record WHERE those files are,
so a later session can find the evidence again. `roadmap_log op:"append"`
(and `op:"append_batch"`) accept an optional `evidence:[paths]` array,
rendered as an `Evidence: path1, path2` body line and echoed by
`roadmap_query` as an `evidence` array.

## Invariants

- **INV-1** — `op:"append"` with `evidence:["a/IMG_1.jpg","logs/run.txt"]`
  writes an `Evidence: a/IMG_1.jpg, logs/run.txt` continuation line into
  the bullet body, with NO trailing sentence period (paths contain dots).
- **INV-2** — `RoadmapDialog::parseBullets` round-trips the line:
  `BulletRecord.evidence == ["a/IMG_1.jpg", "logs/run.txt"]`, dots intact
  (not truncated at the first `.`).
- **INV-3** — a bullet with no `evidence` arg writes no `Evidence:` line
  and `parseBullets` leaves `evidence` empty (additive, common case
  unchanged).
- **INV-4** — a path containing a comma or newline is folded to spaces so
  the single-line `Evidence:` field shape is preserved.

## Test
`tests/features/roadmap_log_evidence/` (label `features;fast`), driving
`RemoteControl::cmdRoadmapLogAppendForTest` against a seeded temp ROADMAP
and `RoadmapDialog::parseBullets` on the result. Verify each behavioural
test fails against pre-ANTS-3382 source (the arg was ignored, no line
written, `evidence` field absent).
