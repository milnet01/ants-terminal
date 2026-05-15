# Feature: Lua sandbox sticky-kill defeats pcall-nesting wall-clock evasion

Regression test for the contract documented in
[`docs/specs/ANTS-1332.md`](../../../docs/specs/ANTS-1332.md). See that
spec for the threat model and full set of invariants — this file
records only what the test exercises and the bounds it asserts.

## Why this test exists

ANTS-1172 added a wall-clock budget to the Lua sandbox, but the budget
could be defeated by a plugin that catches the `luaL_error` longjmp
inside a Lua-level `pcall`. The loop form
(`while true do pcall(function() <busy> end) end`) makes the plugin
recoverable indefinitely — pre-fix, the engine's outer `lua_pcall`
never returns. ANTS-1332 adds:

- a sticky `m_killed` latch the hook checks before any wall-clock
  computation,
- a `lua_sethook` re-arm that drops the count-mask threshold to 1 once
  latched, so each catch is followed by at most one VM
  instruction / one line-change before the next forced fire.

This test catches a regression where any of those guarantees is
re-broken.

## Invariants under test

(IDs correspond to ANTS-1332.md § 4 Invariant N where applicable.)

### Runtime checks

- **A1 — Loop-nested catch terminates within slack.** Load a script
  that runs `while true do pcall(function() <busy work> end) end`.
  Measure `loadScript`'s wall-clock. Assert ≤ `budget + 500 ms`. The
  500 ms slack is for loaded-CI scheduler jitter; the actual unwind
  is sub-millisecond. The production budget is 1500 ms; the test
  injects a tightened budget via `LuaEngine::setPcallBudgetMs()` so
  the assertion completes in well under one second per case (the
  kill path is what's under test, not the wall-clock value itself).
  **If pre-fix code is present, `loadScript` will never return —
  CTest TIMEOUT on the bundle is the backstop.**
- **A2 — Source-nested catch terminates within slack.** Recursive
  100-deep `pcall` nesting (per ANTS-1332 § 5 and the ROADMAP entry's
  explicit ask), with the busy work at the innermost level. Same
  timing bound as A1.
- **A3 — Recovery: a fresh pcall after kill runs normally.** After A1
  returns, load a second script that prints "ok" and writes a
  registry-scoped flag. The script must load and run — proves the
  latch reset in `startPcallBudget`.
- **A4 — Benign plugins not penalised.** Load a script that runs ~10
  000 instructions of trivial Lua and returns. Wall-clock ≤ 100 ms.
  Proves the count-1 latch isn't sticky across pcalls (Invariant 7).

### Source checks

Mirrors of the implementation surface — fail fast if the fix is
removed even when the runtime tests happen to pass on a fast
machine.

- **S1** — `luaengine.h` declares `bool m_killed`.
- **S2** — `luaengine.cpp` resets `m_killed = false` inside
  `startPcallBudget`'s body.
- **S3** — `m_killed = true` is set inside
  `LuaEngine::instructionHook` (the named static function lifted
  from the captureless lambda per ANTS-1332 Invariant 5).
- **S4** — The latched hook re-arms with
  `lua_sethook(L, instructionHook, LUA_MASKCOUNT | LUA_MASKLINE, 1)`
  inside `instructionHook`.
- **S5** — `startPcallBudget` re-arms with
  `lua_sethook(... LUA_MASKLINE, 100000)` so the count is restored
  before the next pcall.

## How the test catches a pre-fix regression

The cleanest pre-fix signal is the A1 hang. With the bug present, the
loop-nested attack never returns from `lua_pcall`; the test process
sits inside `loadScript` forever. We rely on the bundle's CTest
TIMEOUT (30 s — set in `CMakeLists.txt` alongside the bundle's
`set_tests_properties`) to kill the run.

Post-fix, A1 returns in well under 2 s and the entire test bundle
completes in a few seconds.

## What this test does NOT cover

- The `lua_sandbox_hardening` invariants (`string.dump`,
  `luaL_loadfilex("t")`, hook clear before `lua_close`) — that's the
  sibling test in the same `test_lua` bundle.
- Cross-engine interactions — each `LuaEngine` is per-plugin, so a
  rogue plugin can only stall its own VM. The trust contract under
  test here is the per-VM bound, not multi-VM safety.
- `m_pcallBudgetMs` tuning for production callers. The spec considers
  that out of scope; the production default is 1500 ms. The test
  injects a smaller value via `setPcallBudgetMs()` purely to keep
  CI fast — the kill-path semantics are independent of the budget
  value.
