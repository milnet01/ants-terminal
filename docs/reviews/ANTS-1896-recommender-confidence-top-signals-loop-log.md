# ANTS-1896 — Recommender confidence + top_signals refactor — review loop log

Record for [`../specs/ANTS-1896-recommender-confidence-top-signals.md`](../specs/ANTS-1896-recommender-confidence-top-signals.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 — 2026-05-28
- **HIGH H1**: ANTS-1895 `mode` field hard-dep clarified with
  fallback to `switch_enabled:bool`.
- **HIGH H2**: INV-10 wording re-framed against existing
  `test_model_recommender.cpp` tier assertions.
- **HIGH H3**: subsystems.md row references verified
  (modelrecommender L152, modelautoswitch L168, modelnearmissledger
  L192 — all exist).
- **MED M1-M5**: OQ-2 false-positive broadened to all 5 stems;
  INV-3 saturation worked example; override path single-entry
  topSignals; m_lastModelRec normative declaration;
  confidence_floor envelope + typed accessor.
- **LOW**: INV-5 default-initialiser note; RAM byte budget; INV-12
  rounding; slider step 10.

### Loop 2 — 2026-05-28
- **HIGH H1** (ledger filename `model-switch-ledger.jsonl`):
  CHANGELOG hyphenated.
- **HIGH H2** (kSignalNames vs `default` reason): added superset
  note distinguishing taxonomy from reason field shapes.
- **HIGH H3** (saturation unreachable at × 33): multiplier changed
  to × 34 so v1 max sc=6 reaches confidence 100.
- **MED**: INV-11 first-paint clarification; commit_intent override
  size-5 vs size-6 (kept as size-6 with override entry); fraction
  override case in INV-9; Settings → General (not AI); cross-ref
  status note for ANTS-1893/1895.

### Loop 3 — 2026-05-28
- **HIGH** (new): negative-side confidence ceiling asymmetry
  documented (max Haiku confidence is 34 under v1); §2.2 step 5
  empty-window short-circuit semantics clarified (override fires
  BEFORE the empty-turns short-circuit); INV-13 tick-order added.
- **MED**: `test_decide_all_blockers.cpp` coordinated edit; UB
  hand-wave removed.

### Loop 4 — 2026-05-28
- **HIGH** (new): coordinated edits to ANTS-1894 INV-9 + test
  files specified explicitly; supersedence gate documented (then
  softened in loop 6).
- **MED**: kSignalNames "v1 = 6 entries" reconciled (5 scoring
  rules + 1 override sentinel); commit_intent override-path
  fraction=1.0 documented.

### Loop 5 — 2026-05-28
- **HIGH** (new): negative-side asymmetry explicit in INV-3;
  empty-window override semantics; mtime double-scoring claim
  refined.
- **MED**: `Config::claudeAutoModel()` extension snippet for
  confidence_floor; CHANGELOG split.

### Loop 6 — 2026-05-28
- **HIGH** (new): §2.5 master-switch vocabulary made agnostic
  (pre-1895/post-1895); "never-scored" discriminator fixed to
  `topSignals.isEmpty()` alone (not `currentModel.isEmpty()`,
  which misfires on fresh-session `commit_intent` override).
- **MED**: self-declaration of supersedence dropped (§8
  coordinated edits ARE the supersedence); "cheaper short-circuit"
  rationale corrected (decide is evaluate-all post-ANTS-1894).

### Loop 7 — 2026-05-28
- **CRIT C1**: `src/mainwindow.cpp:783-798` connects
  `refreshModelStateChip` BEFORE `refreshAutoModelSwitch` —
  OPPOSITE of INV-13. Added §8 normative wiring change to
  reverse the two connect calls.
- **HIGH H1**: double-scoring on steady-state ticks clarified
  (mtime short-circuit returns BEFORE tooltip composition; spec
  reframed).
- **HIGH H2/H3**: m_lastModelRec freshness rationale and INV-3
  wording polished.
- **MED**: chip hide-on-empty-currentModel reconciled with INV-1
  topSignals.isEmpty() discriminator.
- User gated stop at loop 7. Remaining LOW/INFO carry forward.
