# ANTS-1359 — `verify_changes` session build-cache conformance

Behavioural feature test for the session-scoped cache wrapping
`mcp__ants__verify_changes`. Spec: `docs/specs/ANTS-1359.md`.

## Invariants covered

- **INV-1 (byte-identity)** — hit returns the same `gates` object the
  fresh run did, plus `cache_hit:true`. (HitWithinTtl)
- **INV-2 (TTL)** — entries older than the TTL miss. (Driven via
  direct cache-API put with a manually-rewound `stampMs`.)
- **INV-3 (no mid-run contamination)** — INV-3 framed at the helper
  level: an entry whose pre-snapshot differs from the post-snapshot
  is not inserted. Direct-API LRU/insert tests prove the eviction
  path; the snapshot drift is exercised indirectly through the
  exclusion-list test below (a `bad_config` returned from a fresh
  run is not in the cache).
- **INV-4 (failure classes excluded)** — `bad_config`, `none`,
  `verify_untrusted`, non-git, command-not-resolvable, timeout-killed
  each bypass insert.
- **INV-5 (git-snapshot invalidation)** — tracked-content edit between
  two calls busts the cache; untracked-file creation busts; an empty
  commit busts.
- **INV-6 (cap + LRU)** — cap 8; insert past cap evicts the LRU
  entry; hit on an entry bumps it to MRU.
- **INV-7 (trust-state binding)** — driven via the `cfgSource` +
  `verifyUntrusted` key-material: the key material differs when the
  trust outcome flips, so the same project state hits a different
  key after a trust grant.
- **INV-8 (force_refresh)** — bypasses lookup; runs gates; inserts on
  success.
- **INV-9 (cache_only + incompatible-args)** — `cache_only:true` on a
  miss returns the cache_miss envelope without running gates; both
  flags together return `incompatible_args`.
- **INV-11 (reentrancy)** — flag set across a call; a synthetic
  second call mid-flight returns `verify_in_flight`.

## Out of scope

- End-to-end `cmdVerifyChanges` driven through a full MainWindow.
  Tests use `cmdVerifyChangesWithRoot(root, req)` against a
  `QTemporaryDir` git project to avoid that cost.
- Test 13 (TimeoutKilledNotCached) takes ~10 s wall-clock against the
  real `minTotalTimeoutSec` floor — kept fast by using `/bin/true` for
  the cheap cases and timing out only one slow test.
- Trust-modal interaction (`FakeTrustClient` is used for trust-state
  tests; the real modal client is exercised by the
  `verify_trust_gate` test bundle).
