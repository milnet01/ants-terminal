# ANTS-1359 — `verify_changes` session build-cache — review loop log

Record for [`../specs/ANTS-1359-verify-changes-session-build-cache.md`](../specs/ANTS-1359-verify-changes-session-build-cache.md) § 8. Loop log.

## Cold-eyes loop log

- **Draft 2026-05-15** — initial spec.
- **Cold-eyes pass 1 (2026-05-15)** — two parallel reviewers
  surfaced 3 CRITICAL (`.gitignore`d input invisibility; pre/post
  race-window wording; Qt event-loop reentrancy through
  `QProcess::waitForFinished`); 4 HIGH (HEAD-vs-tree, trust transition
  not in key, timeout-killed gates cached, env var not in key); 5
  MEDIUM (TTL justification, test-6 `touch` bug, force+cache_only
  combo undefined, missing trust-transition test, missing reentrancy
  test); + cross-doc issues (fabricated tokenusageengine 8192-as-wire-
  bytes claim, fabricated peer-cache RAM numbers, ambiguous cache
  ordering vs trust client). All verified findings folded in: key now
  carries trust outcome + autotrust env; § 2.5 grew to 7 exclusion
  classes; INV-11 reentrancy gate added; § 1 token framing reframed
  to compute-axis; § 2.9 dropped unsourced peer numbers; § 3.1 grew
  from 17 → 22 tests covering the new INVs.
- **Cold-eyes pass 2 (2026-05-15)** — second-pass reviewer verified
  pass-1 fixes landed correctly AND surfaced 2 NEW HIGH (key-time
  `outcomeForConfig` call could trigger an unwanted modal on fresh
  SHAs because pass 1 wrongly claimed "no prompt"; reentrancy flag
  leaked through the existing `bad_config` mid-function early return
  with no RAII guard) + 2 MEDIUM (test 13 `timeout_sec:1` clamped
  by `minTotalTimeoutSec` floor — unreachable via public MCP args;
  test 21 needs explicit `QTest::qWait` event-loop pump). All
  folded: § 2.1 reordered so `loadGateConfig` runs at step 6 BEFORE
  the cache key is computed (the trust outcome the cache needs is
  the actual outcome of that call, not a hypothetical peek);
  § 2.7 control flow now uses `qScopeGuard` for the reentrancy
  flag (resets on every return path); test 13 rewrites to use
  `sleep 15` + `timeout_sec:10` against the real floor; test 21
  documents the `QTest::qWait(2000)` pump.
- **Cold-eyes pass 3 (2026-05-15)** — third-pass reviewer verified
  pass-2 fixes landed AND surfaced 1 HIGH (the pseudo-code's
  `runVerify(cfg, opts)` implied a non-trivial VerifyEngine
  signature change not listed in § 4 build cost) + 2 MEDIUM
  (cache-hit cost of step-6 `loadGateConfig` not acknowledged;
  `TrustGrantBustsCache` mechanism under-specified). All folded:
  spec now keeps the existing `runVerify(projectPath, opts)`
  signature and accepts the silent duplicate `loadGateConfig` call
  in the miss path (explained in § 2.1 rationale + § 2.7 comment +
  § 5 as a v2 cleanup); § 2.1 names the sub-millisecond cache-hit
  cost; tests 9 and 10 spell out the `FakeTrustClient` setter
  mechanism.
- **review-contract loop 1 (2026-09-28)** — gating one clause: § 3.1 test
  8, amended for ANTS-1289 INV-14 (ANTS-5498). Two cold `review-lane`
  lanes, all four questions each. **Q1 0 · Q2 0 · Q3 0 · Q4 1** inside
  the change — verified 1, fixed 1: an empty root has no commit, so class
  5 already excluded it and the clause could not fail; now a git repo with
  a commit, asserting `config_source:"none"` (the shipped test already
  did both). Outside the change, verified and filed rather than looped
  on: force_refresh never inserts in § 2.7 against INV-8 (Q2), the 300 s
  TTL value is untested (Q4), test 13's self-contradiction, and whether
  class 2 should key on `ran_count == 0` — ANTS-5526. Lane spend 93.8k and
  92.2k tokens cumulative.
- **review-contract loop 2 (2026-09-28)** — same gate, cold re-read, two
  lanes with all four questions. **Q1 0 · Q2 0 · Q3 0 · Q4 0** inside the
  change: converged. Both lanes checked test 8 against class 5, § 2.7's
  insert condition and the envelope, and found it can fail. Outside the
  change they re-found ANTS-5526's force_refresh (Q2) and TTL (Q4) items,
  already filed. Lane spend about 93k tokens each, cumulative.
- **impl fold-back (2026-11-13)** — ANTS-5526, no reviewer dispatched. The
  code had § 2.7's `&& !force`, so a forced run never inserted; fixed in
  code and pseudocode to match INV-8, with test 23 red first. Test 2 now
  states what the shipped `MissAfterTtl` does and checks the 300 s value.
  Test 13 restated. § 2.5 class 2 records why it keys on `config_source`.
