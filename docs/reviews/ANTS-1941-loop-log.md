# ANTS-1941 behaviour epoch — review record

The loop log for [`docs/specs/ANTS-1941-behaviour-epoch-boundary-model-switch.md`](../specs/ANTS-1941-behaviour-epoch-boundary-model-switch.md).

## Cold-eyes loop log

Two independent cold reviewers per loop (no prior-loop briefing); every verified
CRITICAL/HIGH/MEDIUM/LOW fixed in-place, then re-read cold.

- **Loop 1 (2026-06-02)** — 0 CRITICAL / 0 HIGH. Fixed 6 citation-precision items:
  §1 age-filter cite (`:539` → `:539-545`/drop `:542-543`); §2.4 regret-guard cite
  (`:425-426`); §2.4 strict-`<` (epoch ≥ minEpoch in-scope, never `!=`); §7 INV-7
  credited as compile-time `static_assert`; §2.4 age-block elision annotated; §4
  concurrency one-liner.
- **Loop 2** — 2 HIGH: the `static_assert` INV-7 mandates was **absent** from the
  §2.1 code block (added); §2.5 "both readers agree" needed explicit `minEpoch` at
  both sites + the latent `windowDays` default-reliance flagged. Plus: §1 sourced
  the 62.5 % figure; §2.4 dial-neutral claim qualified; emit-when-count-0 stated;
  §2.3 sole-append-site marked a process rule; §2.2 JSON key position; §2.5 stale
  comments `:557`/`:559`.
- **Loop 3** — 2 HIGH: the outcome fill-in `writeRecords` (`:2204`) round-trip
  preserves `epoch` (was unstated — silent-data-loss path; now tied to INV-1/2);
  the "robustness half" had no covering test (added the composition note). Plus §1
  percentage density compressed; §2.4 struct insertion point; INV-3 epoch-above-
  minEpoch case.
- **Loop 4** — 2 HIGH: `Kind: enhancement` → `implement` (taxonomy); the loop-3
  §7 composition note cited `measured=4` rows as if `measured=0` — added **INV-8**
  (all-pre-epoch → `measured_downgrades=0`, `regret_rate=0.0`) and repointed to the
  `measured`-independent branch at `:161`. INV-7 checkable/process split;
  `kSwitcherEpoch` namespace; filed **ANTS-1942** (windowDays asymmetry) +
  **ANTS-1943** (Kind taxonomy ↔ tool-enum drift).
- **Loop 5** — 0 CRITICAL / 0 HIGH. INV-3 positive-survival assertion; §7
  "measured-independent" reworded to the `measured < floor ∧ regret ≤ 40`
  condition; INV-7 ↔ §7 single-sourced; §8 records the ANTS-1942/1943 filing. One
  finding dismissed **unverified** — both reviewers called ANTS-1942 a phantom ID;
  confirmed it exists (ROADMAP.md:8978), they read stale file state.
- **Confirming loop (2026-06-02)** — 0 CRITICAL; no new contract defects (one
  reviewer 0 HIGH, contract called sound). Polish-only, all fixed: named
  `kRegretConservatismPct = 40` (`modelautoswitch.h:198`); qualified the
  `test_model_auto_switch.cpp` path; pinned epoch-key INVs to `statsForScope` (the
  only key emitter); noted the `conservatismMultiplierFor` 60 s TTL cache; marked
  JSON key order cosmetic; this Status/date + log populated.
- **impl (2026-11-15)** — no reviewer was dispatched; implementation fold-back. ANTS-6229: `conservatismMultiplierFor` now caches the ledger's inputs, not the multiplier, so § 2.5's note on the 60 s TTL says so.
