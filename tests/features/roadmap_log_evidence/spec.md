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
- **INV-5** (ANTS-3407) — the `^`-anchored metadata labels tolerate any
  case: a hand-edited lowercase `kind:` / `evidence:` line parses the same
  as the canonical capitalised form the writer emits (parity with the
  long-standing `Layman:` tolerance). The un-anchored `Lanes:` label
  deliberately stays case-SENSITIVE — case-insensitivity there would
  mis-capture a lowercase `"lanes:"` occurring mid-prose — so a lowercase
  `lanes:` is left unparsed.

- **INV-6** (ANTS-4527) — an `Evidence:` element that is not path-shaped is
  reported to the caller as an `evidence_not_path_shaped` advisory in the
  envelope's `warnings` array. `roadmap-format.md` § 3.5 defines every
  element as a path, and the field is comma-split, so prose written here is
  not merely unresolvable — it is stored as several fragments rather than as
  the sentence its author wrote.
- **INV-7** (ANTS-4527) — the control: a real path raises NO advisory,
  whether it carries a separator (`photos/IMG_2031.jpg`) or only an
  extension (`shot.png` at the repo root). The extension half is why this is
  not ANTS-4502's separator-only predicate, which would have flagged the
  second.
- **INV-8** (ANTS-4527) — prose carrying its own commas is reported. This is
  the measured corpus shape: one sentence produced three stored elements,
  `96`, `000 mutants across all twelve parsers under ASan+UBSan` and
  `clean`, each individually plausible and none of them what was written.

It is an ADVISORY on a successful write, never a refusal. `roadmap_log` is
called by sessions across every project on this machine; a new refusal on
input accepted today would break them, and the measured defect covers a
handful of items. The advisory fires on both the store and markdown write
paths — one that fired only on migrated projects would be a worse
inconsistency than the defect it reports.

## Test
`tests/features/roadmap_log_evidence/` (label `features;fast`), driving
`RemoteControl::cmdRoadmapLogAppendForTest` against a seeded temp ROADMAP
and `RoadmapDialog::parseBullets` on the result. Verify each behavioural
test fails against pre-ANTS-3382 source (the arg was ignored, no line
written, `evidence` field absent).
