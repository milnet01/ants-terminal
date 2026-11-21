# ANTS-1880 — `spec_query` + `project_layout` recognise `docs/phases/` — review loop log

Record for [`../specs/ANTS-1880-spec-query-project-layout-recognise.md`](../specs/ANTS-1880-spec-query-project-layout-recognise.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 2 (2026-05-26).** Reviewer flagged 0 CRITICAL, 3 HIGH
  (`source` field "optional" vs INV-3 "always emitted"
  contradiction; kProbeSetVersion bump didn't pin the History:
  comment-block location; §2.3 silently redefined `specs_scanned`
  — back-compat hazard), 4 MEDIUM (INV-1 example `phase_01_intro`
  vs §Layman `_design` suffix; phases probe-order cross-ref
  unstated; no-short-circuit reads as new contract not
  preservation; §4 Tests undercounted INVs at 1..4 vs 1..6). All
  verified against `projectlayoutengine.h:36-40` (History
  comment), `remotecontrol.cpp:6934` (specs_scanned emit). Fixes:
  drop "optional" qualifier from §2.1; pin the History:
  comment block location for the bump; keep `specs_scanned`
  semantics, add `phases_scanned` + `total_scanned` siblings;
  clarify INV-1 examples; cite `kSpecsCandidates` ordering
  precedent; reword §2.3 no-short-circuit as preservation;
  rewrite §4 Tests with full INV-1..INV-6 anchor coverage +
  bundle-placement note.
- **Loop 1 (2026-05-26).** Reviewer flagged 0 CRITICAL, 4 HIGH
  (probe-set version bump not specified — silent cache contract
  drift; JSON schema for new `phases_dir` field undefined;
  INV-1 regex contradicted dir routing — needed disjoint regex per
  dir, not fallback; `cmdInvariantCheck` merged response shape
  unspecified), 5 MEDIUM (regex too permissive — tightened to
  `^phase_[0-9]+_[a-z0-9_]+$`; standards.md impact understated;
  cold-eyes log empty; etc.). All verified against
  `src/projectlayoutengine.h:40` (kProbeSetVersion contract),
  `src/projectlayoutengine.cpp` (scanDir / first-hit), Vestige's
  example file pattern. Fixes: bump `kProbeSetVersion` 3→4 in
  INV-5; pin JSON shape (`phasesDir`/`phases_dir`, empty-default,
  isStale parity); tighten regex; split INV-2 into per-shape
  routing with explicit "no cross-dir fallback"; add `source`
  field response echo (INV-3); pin `cmdInvariantCheck` merged
  shape with `phases_scanned` sibling diagnostic; renumber INVs
  to accommodate the additions.
