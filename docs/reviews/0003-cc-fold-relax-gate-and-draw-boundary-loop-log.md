# ADR-0003: Relax the ANTS-1120 gate for the /audit /indie-review /debt-sweep folds, and draw the ANTS-1108 ↔ ANTS-1111 / ANTS-1113 boundary — review loop log

Record for [`../decisions/0003-cc-fold-relax-gate-and-draw-boundary.md`](../decisions/0003-cc-fold-relax-gate-and-draw-boundary.md) § Cold-eyes review pass.

## Cold-eyes loop log

This ADR was self-reviewed before sign-off, then folded back into
the cold-eyes loop on `docs/specs/ANTS-1111-fold-audit-triage-into-project.md` (2026-05-13).

### Findings raised (self-review)

- D-1: ANTS-1120 was at risk of silent retirement; the original
  draft did not specify what becomes of its measurement mandate.
- D-2: Boundary statement was bullet-bullet ("ANTS-1108 vs
  ANTS-1111"), making implementers diff full ROADMAP bodies to
  find which bullet owns a given feature.
- D-3: The "what already exists" inventory was at risk of
  re-inventing scaffolding (the original draft did not enumerate
  shipped helpers, opening the door to a parallel allowlist
  implementation).
- D-4: The docs-level allowlist (`docs/audit-allowlist.md`)
  reference would have required a non-shipped dependency
  (ANTS-1107) without a named bypass.

### Resolutions

- D-1 Resolved: ANTS-1120 scope reframed in decision 1 (post-hoc
  measurement instead of pre-ship gate; concrete retire criterion
  of < 5 % over 7 days).
- D-2 Resolved: decision 2 replaced the bullet-bullet wording
  with a per-surface table (Workflow panel vs triage logic vs
  debt-sweep detectors vs indie-review orchestration) so future
  implementers have a per-feature lookup.
- D-3 Resolved: decision 4 enumerates pre-existing scaffolding
  before any spec is written. Counter-checked against
  `auditdialog.h`, `auditengine.h`, `featurecoverage.h`,
  `audithygiene.h` on 2026-05-13.
- D-4 Resolved: decision 5 declares the docs-level allowlist
  out-of-scope for this ADR's bullets, with a named follow-up
  trigger (ANTS-1107 ship). No silent dependency on a
  non-shipped item.

### Findings raised (cold-eyes loop on the paired spec, 2026-05-13)

- HIGH: dec 4 listed `severityShiftForCorroboration` and
  `applyAllowlistDrops` as new pure functions; the spec uses
  `applyCorroborationShift` and never defines
  `applyAllowlistDrops` (the actual change is to widen the
  existing call site).
- LOW: dec 4 said "RoadmapFoldIn calls into the existing flock
  mechanism" — there is no existing flock mechanism on
  `.roadmap-counter`; the helper introduces it, adopting the
  `configbackup.h` pattern.

Both resolved in-place by amending decision 4. No further
substantive findings — the ADR's body remains as drafted.
