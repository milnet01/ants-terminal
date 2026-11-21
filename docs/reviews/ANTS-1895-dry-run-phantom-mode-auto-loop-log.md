# ANTS-1895 — Dry-run / phantom mode for the auto-model switcher — review loop log

Record for [`../specs/ANTS-1895-dry-run-phantom-mode-auto.md`](../specs/ANTS-1895-dry-run-phantom-mode-auto.md) § 11. Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 — 2026-05-28
- **CRIT C1** (`emit configChanged()` references non-existent
  signal — Config has no Q_OBJECT): dropped, replaced with the
  existing `storeIfChanged() + save()` pattern.
- **HIGH H1-H3**: `claudeAutoModel()` snippet completeness;
  ANTS-1893 hard-dep declaration; CLAUDE.md anchor by bullet.
- **MED M1-M5**: INV-9 folded into INV-7 (envelope derivation);
  OQ-2/3/4 moved to §9 Design decisions; `seedOverrideCacheFromLedger`
  cool-down filter symmetry; mode-key namespace collision noted;
  INV-13 dwell/stability counter clarification.

### Loop 2 — 2026-05-28
- **HIGH H1** (`setClaudeAutoModelSwitch` example breaks no-op
  short-circuit): rewritten with bitwise-OR fold mirroring
  setWindowGeometry pattern.
- **HIGH H2** (INV-3 PTY mock seam doesn't exist): test approach
  reframed to behavioural observation via
  `lastAutoSwitchTierForTest()` + ledger record assertion + source
  presence check.
- **HIGH H3** (StatsConfig has no `mode` field): added explicit
  field to §2.6.
- **MED**: local-variable name `out` (not `o`); `m_autoSwitchLastTier`
  intentionally not updated under dry-run; INV-6 binds both
  cool-down-seed call-sites; §2.3 vs §1 silent-agreement
  reconciled (only near-miss, never `would_have_fired`);
  OQ-2 → Design decision D-6.

### Loop 3 — 2026-05-28
- **HIGH** (new): downgrade asymmetry documented; ANTS-1896 not
  ROADMAP-only; INV-1 line-cite clarified.
- **MED**: ANTS-1891 INV-5 reference corrected (was "INV-9");
  Settings label→value mapping; test-count 13.

### Loop 4 — 2026-05-28
- **CRIT C1** (INV-3 references non-existent
  `lastAutoSwitchTierForTest`): accessor declared in §2.4.
- **HIGH**: would_have_weighted_avoided + dry_run_headline_floor
  prefix discipline; `decide()` short-circuit framing reworded
  (it's evaluate-all, not return-early).
- **MED**: §2.7 dialog co-edit note for ANTS-1893; INV-12 dwell
  reset-on-firing carry-across-flip distinction; M-5 config-key
  rename considered but kept for back-compat.

### Loop 5 — 2026-05-28
- **HIGH** (new): INV-13 prefix rename
  (`would_have_avoided_opus_turns` + `would_have_weighted_avoided`);
  consumer key `dry_run_headline_floor` clarification.
- **MED**: INV-12 wording sharpened on flip-vs-firing.

### Loop 6 — 2026-05-28
- **HIGH** (new): `would_have_clean_end_count` added to envelope
  example + INV-13 algebra; §2.7 co-edit note with ANTS-1893.

### Loop 7 — 2026-05-28
- **HIGH** (new): dry-run "measured" rule (different from live's
  `turnsOnToTier > 0` gate) defined to use settled+any-signal —
  prevents zero-denominator collapse.
- **MED**: settlement rule for subsequentAutoSwitches; dialog
  timing during merge.
- User gated stop at loop 7. Remaining LOW/INFO carry forward.
