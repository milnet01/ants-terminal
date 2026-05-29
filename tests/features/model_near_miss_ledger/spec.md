# model_near_miss_ledger — feature-conformance spec

Covers ANTS-1894 INV-1..INV-11 + INV-13 (INV-12 lives in the MCP-side
test under `mcp_model_switch_stats_near_misses/`). Pure C++ tests
against `ModelNearMissLedger`, `ModelAutoSwitch::decide`, and the
controller's near-miss emit path.

## Invariants

- **INV-1** — `decide()` returns `act=true` iff `blockedBy.isEmpty()`;
  on act, `tierArg == tierName(recommendedTier)`. Preserves the
  pre-1894 firing-side contract.
- **INV-2** — Each of the 8 guards in § 2.1 appends its canonical token
  on failure; tokens land in evaluation order. The v1 7-token taxonomy
  is never renumbered; ANTS-1917 appended `idle_end_of_session` as the
  8th token (kept last). A default-constructed Gate still yields exactly
  the v1 7 tokens because the idle guard is opt-in via the -1 sentinel.
- **INV-3** — `effectiveMinDwellMs(g) = max(kMinDwellMs,
  g.configuredMinDwellMs)`; degenerate `configuredMinDwellMs=0` still
  respects the 90 s floor.
- **INV-4** — `Decision.currentTier` and `recommendedTier` set on every
  call regardless of act; `recommendedTier` is the *clamped* target.
- **INV-5** — `maybeEmitNearMiss` writes iff the post-sort blocked_by
  signature differs from the last emitted signature for this project.
- **INV-6** — `maybeEmitNearMiss` skips when `nowMs - lastEmitMs <
  kNearMissEmitFloorMs` (5 s); skipped attempts do NOT update the
  throttle state.
- **INV-7** — `defaultLedgerPath()` resolves under XDG_CACHE_HOME's
  `ants-terminal/model-switch-nearmiss.jsonl`; mode 0600; one
  `\n`-terminated JSON per line.
- **INV-8** — `appendRecord` post-cap-overflow evicts whole oldest
  lines; newest line never dropped; no mid-line truncation.
- **INV-9** — The 8 blocker tokens are stable handles (source-grep
  sentinel for accidental renames); the v1 7 are never renumbered and
  `idle_end_of_session` (ANTS-1917) is appended last.
- **INV-10** — `statsSlim.dominantBlocker` = highest-occurrence token
  across the 24 h window's records (one count per distinct token in a
  record's `blocked_by`); ties broken by taxonomy order; empty window
  ⇒ empty string.
- **INV-11** — `statsFull.window_24h.total` filters by ts ≤ 24 h before
  `nowMs` and project match; `all_time` has no window filter;
  `distinct_signatures` counts post-sort distinct blocked_by arrays.
- **INV-13** — `fromJson` of legacy JSON missing optional fields
  returns Record with struct-default fallbacks; idempotent
  re-serialisation holds for writer-produced records.

## Test files (12; bundle membership in CMakeLists.txt)

- `test_decide_act_iff_unblocked.cpp` → `test_core` (INV-1)
- `test_decide_all_blockers.cpp` → `test_core` (INV-2)
- `test_effective_min_dwell.cpp` → `test_core` (INV-3)
- `test_decision_tiers_always_set.cpp` → `test_core` (INV-4)
- `test_emit_on_sig_change.cpp` → `test_claude` (INV-5; drives ClaudeStatusBarController)
- `test_emit_floor_throttle.cpp` → `test_claude` (INV-6; drives ClaudeStatusBarController)
- `test_ledger_file_shape.cpp` → `test_core` (INV-7)
- `test_evict_to_cap.cpp` → `test_core` (INV-8)
- `test_blocker_taxonomy.cpp` → `test_core` (INV-9; source-grep sentinel)
- `test_stats_slim_dominant.cpp` → `test_core` (INV-10)
- `test_stats_full_windows.cpp` → `test_core` (INV-11)
- `test_json_roundtrip.cpp` → `test_core` (INV-13)

Per the project test convention, every test is verified to FAIL
against pre-1894 source before the fix is restored.
