# ANTS-1332 — Lua sandbox sticky-kill defeats pcall-nesting wall-clock evasion

**Status:** spec → test → implement
**Kind:** security
**Source:** indie-review-2026-05-14 (lane-6 H-1)
**Pairs with:** ANTS-1172 (the original wall-clock budget added the
behaviour this spec hardens). The new regression test bundles into
the existing `test_lua` CMake target alongside
`tests/features/lua_sandbox_hardening/` and
`tests/features/plugin_manifest_safety/` (see
`CMakeLists.txt:982–991`, the `ants_add_gui_bundle(test_lua ...)`
block).

---

## 1. Summary

Once `LuaEngine`'s wall-clock budget for a `pcall` is breached, the
engine MUST guarantee bounded-duration termination regardless of how
the plugin nests `pcall` internally. The instruction hook currently
calls `luaL_error` on each fire when `wallExpired` is true — but
between fires the plugin still gets one full instruction-count slice
(~100 000 Lua instructions) of work, and an inner `pcall` swallows
each longjmp. A plugin that spins `while true do pcall(work) end`
runs unbounded slices until C-stack exhaustion eventually frees the
UI thread. Fix: a sticky `m_killed` flag, set at first timeout, that
makes every subsequent hook fire raise `luaL_error` AND drops the
hook count to 1 so each catch is followed by a single-instruction
gap. The flag clears in `startPcallBudget()` (next outer `lua_pcall`)
and in `shutdown()`.

## 2. Problem statement (threat model)

The Lua sandbox is the boundary between user plugin code and the
Ants Terminal main (UI) thread. The trust contract is: a malicious
or buggy plugin can take down its own VM (memory limit, instruction
hook, wall-clock budget), but MUST NOT freeze the UI thread for
longer than `kPcallBudgetMs` plus a small slack window for the
final unwind.

The wall-clock branch was added in ANTS-1172 specifically because
the instruction-count hook cannot fire while pure-C code is
executing — `string.gsub` with a catastrophic regex, `table.sort`
with a pathological comparator, etc. ANTS-1172's wall-clock check
inside the count+line hook closes the gap *for code that
eventually returns to bytecode*.

What ANTS-1172 does NOT close is the case where the plugin author
intentionally catches the timeout with a Lua-level `pcall`:

```lua
-- attack pattern
while true do
  pcall(function()
    -- busy work that consumes the next ~100 000 Lua instructions
    local x = 0
    for i = 1, 1e9 do x = x + 1 end
  end)
end
```

Trace with the current code:

1. Plugin enters the loop. ~100 000 instructions run, then the
   count hook fires.
2. Hook computes `wallExpired = nowMs > m_pcallDeadlineMs`. True.
3. Hook sets `m_timedOut = true`, calls `luaL_error`. longjmp
   unwinds to the **inner** `pcall`.
4. Inner `pcall` catches the error string, returns. The outer
   `while true` continues.
5. New iteration: ~100 000 instructions run before the next hook
   fire. Plugin made another 100 000 instructions of progress.
6. Hook fires. `wallExpired` is still true. luaL_error → inner
   pcall catches. Loop.

Each cycle = ~100 000 instructions of useful work, ≈ 0.1–1 ms
wall-clock per cycle on modern hardware. Steady-state freeze
duration scales with the count-threshold size — not "bounded by
`kPcallBudgetMs`" as the `kPcallBudgetMs` comment at
`luaengine.cpp:137–141` would lead a reader to expect.

There are two attack patterns and they terminate differently
under the current code:

- **Source-nested pcalls** (`pcall(function() pcall(function()
  pcall(function() … end) end) end)` to many levels) — each
  level consumes a C-stack frame. Lua's `LUAI_MAXCCALLS` C-call
  depth limit (defined in the Lua 5.4 source `lstate.h`,
  upstream default `200`) eventually fires an uncatchable C-call
  overflow error that propagates out to the engine's outer
  `lua_pcall`. The ROADMAP entry's "~5 minutes before C stack
  pressure forces an escape" framing applies to this form — it
  *does* terminate, just slowly.
- **Loop-nested pcalls** (`while true do pcall(function() …
  end) end`) — each iteration creates and destroys one
  protected frame, so the C stack does *not* accumulate and
  `LUAI_MAXCCALLS` never fires. This form does **not**
  terminate under the current code; the plugin runs forever
  until the user kills the terminal. This refines (and
  strengthens) the ROADMAP entry's bound.

## 3. Surface — current state

`src/luaengine.cpp:73–133` (`initialize` + the lua_sethook callback)
and `src/luaengine.cpp:135–144` (`startPcallBudget`):

```cpp
// initialize() — current hook
lua_sethook(m_state, [](lua_State *L, lua_Debug *) {
    lua_getfield(L, LUA_REGISTRYINDEX, "__ants_engine");
    auto *eng = static_cast<LuaEngine *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!eng) {
        luaL_error(L, "Script execution timeout exceeded");
        return;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool wallExpired =
        eng->m_pcallDeadlineMs > 0 && nowMs > eng->m_pcallDeadlineMs;
    if (wallExpired) {
        eng->m_timedOut = true;
        luaL_error(L, "Script wall-clock budget exceeded");
    }
}, LUA_MASKCOUNT | LUA_MASKLINE, 100000);
```

Note the hook is set with `LUA_MASKCOUNT | LUA_MASKLINE`: it fires
on every 100 000th VM instruction OR on every Lua source-line
change, whichever happens first. The 100 000 figure is the
*count-mask* threshold; the line-mask fires independently.

State fields (`src/luaengine.h:123–124`):

```cpp
bool m_timedOut = false;       // set by hook; checked after pcall
qint64 m_pcallDeadlineMs = 0;  // wall-clock deadline (ms epoch)
```

`m_timedOut` is read+cleared at `fireEvent` (`luaengine.cpp:342, 357`)
once the outer `lua_pcall` returns. It is a one-shot signal, not a
"stay killed" latch.

## 4. Contract — invariants

### Invariant 1 — sticky kill latch exists

A `bool m_killed` member exists on `LuaEngine`, default-initialised
to `false` at its declaration in `luaengine.h` (matching the
`m_timedOut = false` pattern on the adjacent field), and reset to
`false` in `shutdown()` alongside the existing `m_timedOut`
assignment.

### Invariant 2 — `startPcallBudget()` resets the latch

`startPcallBudget()` sets `m_killed = false` before computing the
new deadline. `fireEvent` already breaks out of its handler loop
on `m_timedOut` (`luaengine.cpp:342, 357`), so the next outer
`lua_pcall` to observe a fresh latch state is either (a) the
next event's `fireEvent` call, (b) a subsequent `loadScript`, or
(c) a manual `startPcallBudget` from any future caller. Without
this reset, a single timeout would permanently kill the VM —
every subsequent pcall would hit the latched-hook path before
making any progress.

### Invariant 3 — hook latches on first wall-clock expiry

The first time the hook observes `wallExpired`, it sets BOTH
`m_timedOut = true` AND `m_killed = true` before calling
`luaL_error`. Order matters: `luaL_error` longjmps, so any field
writes after it are unreachable.

### Invariant 4 — latched hook fires `luaL_error` unconditionally

When `m_killed` is true on entry to the hook, the hook calls
`luaL_error` immediately — no wall-clock check, no engine-pointer
check beyond the basic `eng != nullptr` guard. The plugin gets
at most one VM instruction (or one Lua line-change, whichever
fires next) between catches — see Invariant 5 for the mechanism.

### Invariant 5 — hook count drops to 1 once latched

On the first wall-clock expiry, the hook also calls
`lua_sethook(L, instructionHook, LUA_MASKCOUNT | LUA_MASKLINE, 1)`
to drop the per-fire count-mask threshold from 100 000 to 1. The
line-mask stays set, so the hook re-fires on either the next
instruction OR the next line change — whichever the plugin
reaches first. This bounds the "useful work per nested catch" at
**one VM instruction or one Lua line-change**, not strictly one
instruction — the line-mask makes the bound even tighter in
practice (most line-changes happen well before 1 full
instruction's worth of bookkeeping).

**Ordering within the hook body:** the `lua_sethook` re-arm MUST
be issued BEFORE `luaL_error` — `luaL_error` longjmps, so any
statement after it is unreachable and the latch-tightening would
never take effect.

`startPcallBudget()` restores the count to 100 000 (and keeps the
same mask) by re-calling `lua_sethook` so the next pcall runs at
normal cadence.

The hook callback MUST be referenceable by a stable name from
inside its own body so the re-arm call compiles. The current
captureless lambda in `initialize()` MUST be lifted to a named
private-static `LuaEngine::instructionHook(lua_State *L,
lua_Debug *)` so both `initialize()` and the in-hook re-arm can
pass it to `lua_sethook` by symbol.

### Invariant 6 — total UI-thread stall is bounded by `kPcallBudgetMs + LUAI_MAXCCALLS × t_unwind`

With the latch in place, the worst-case stall for a malicious
plugin is:

- `kPcallBudgetMs` (1500 ms — `luaengine.cpp:142`, a function-local
  `constexpr` inside `startPcallBudget`; not exposed as a class
  symbol, so tests refer to the literal `1500`) — wall budget
  consumed before the first hook fire.
- Plus `LUAI_MAXCCALLS × t_unwind` ≈ 200 × (a few hundred
  nanoseconds for one VM-instruction + one longjmp) ≈ < 1 ms —
  the time to walk down the C stack from the deepest nested catch
  site, since every catch re-fires `luaL_error` after at most one
  instruction/line-change and unwinds one frame.

Total stall: ≤ ~1505 ms regardless of pcall nesting.

### Invariant 7 — successful pcalls still run at full speed

A well-behaved plugin (every handler returns within
`kPcallBudgetMs`) sees zero behavioural change. The hook count
stays at 100 000 between pcalls and `m_killed` is never set.

## 5. Test strategy

A new feature-conformance test at
`tests/features/lua_pcall_nesting_timeout/` covering:

### Runtime checks

1. **A1 — Loop-nested catch terminates within slack** — Load a
   script that runs `while true do pcall(function() <busy>
   end) end`, measure wall-clock from `loadScript` start to
   return, assert ≤ `1500 + 500 ms` (kPcallBudgetMs is a
   function-local constexpr so the test names the literal; 500 ms
   slack covers loaded-CI scheduler jitter, not the unwind itself
   which is sub-millisecond).
2. **A2 — Source-nested catch terminates within slack** — Load
   a script that recursively nests `pcall(function() return
   recurse() end)` to depth 100 (matching the ROADMAP entry's
   regression-test ask), with each level wrapping the recurse
   call in its own `pcall`. Depth 100 is well under Lua's
   `LUAI_MAXCCALLS` default of 200, so the test stresses the
   unwind path without hitting the C-call ceiling as a separate
   exit condition. Same timing bound as A1.
3. **A3 — Recovery: a fresh pcall after kill runs normally** —
   After A1 returns, fire a benign event handler and assert it
   runs to completion (proves Invariant 2: latch resets per
   pcall).
4. **A4 — Benign plugins not penalised** — Load a script that
   runs ~10 000 instructions and returns. Measure wall-clock,
   assert ≤ 100 ms (proves Invariant 7: no count-1 leak).

### Source checks

5. **S1 — `m_killed` member declared on LuaEngine** — grep
   `luaengine.h` for `bool m_killed`.
6. **S2 — `m_killed` reset in `startPcallBudget`** — grep
   `luaengine.cpp` for `m_killed = false` inside
   `startPcallBudget`'s body span.
7. **S3 — Hook latches on first expiry** — grep for the
   `m_killed = true` assignment inside the body of
   `LuaEngine::instructionHook` (the named static function lifted
   from the lambda per Invariant 5).
8. **S4 — Hook count drops to 1 on latch** — grep for
   `lua_sethook(L, instructionHook, LUA_MASKCOUNT | LUA_MASKLINE, 1)`
   inside the hook body (anchored to the mask to avoid matching
   the `lua_sethook(m_state, nullptr, 0, 0)` line in `shutdown`).
9. **S5 — `startPcallBudget` restores count to 100000** —
   grep for `lua_sethook(.*LUA_MASKLINE, 100000)` inside
   `startPcallBudget`'s body span (in addition to the one in
   `initialize`).

### Manual cross-checks (one-off, not in test)

- Build runs with `-fsanitize=address` (default debug preset)
  must not flag the new `lua_sethook` re-arm from within the
  hook lambda. ASan/UBSan is the existing gate; no new
  sanitizer rules.

## 6. RAM / build-cost budget

- Per-`LuaEngine` instance: one extra `bool` (`m_killed`),
  word-aligned with `m_timedOut`. Net change: 0 bytes (fits in
  existing padding).
- Build cost: ~6 added lines in the lifted `instructionHook`
  body (the lift itself per Invariant 5 is a wash on line count
  — the captureless lambda body relocates to a named static),
  ~3 lines in `startPcallBudget`, 1 field, and 1 new declaration
  in `luaengine.h`. Compile-time delta: negligible.
- Per-call cost on the hot path (well-behaved plugin):
  - Hook: one extra `eng->m_killed` load + branch (predicted
    "not taken" by every iteration). Single-digit cycles.
  - `startPcallBudget`: one extra `m_killed = false` store
    + one `lua_sethook` call. The latter touches one cache line
    in the lua_State; ANTS-1172's existing `lua_sethook` from
    `initialize` is the same call, so the per-pcall overhead
    is one C function call (~10 ns).

## 7. Rollback / regression risk

The fix only adds *additional* termination conditions to the
hook. It cannot make a benign plugin terminate earlier than it
would have under the prior code (Invariant 7). Failure modes
considered:

- **`lua_sethook` from inside the hook misbehaves.** Lua 5.4
  documents `lua_sethook` as callable from any thread and at any
  time. The new setting takes effect on the next instruction;
  since the current hook longjmps out via `luaL_error`, the new
  setting is in place when the longjmp lands. Verified against
  the Lua 5.4 reference manual's C-API debug-interface section
  (the chapter documenting `lua_sethook` itself).
- **C-stack unwind during repeated `luaL_error`.** Each catch is
  a longjmp + C-frame pop. Lua's `LUAI_MAXCCALLS` (Lua 5.4
  upstream `lstate.h` default `200`) bounds the depth; once the
  deepest pcall is reached, the next `luaL_error` propagates past
  Lua-script pcalls to the engine's outer `lua_pcall`. Verified
  by A2.
- **Hook count of 1 starves the hot path.** Only after a
  timeout. `startPcallBudget` restores 100 000 before the next
  pcall (per Invariant 5 — `startPcallBudget` restore clause).
  Verified by A4.

## 8. Migration / compatibility

None. Internal-only behaviour change. No plugin-facing API or
on-disk format touched. `PLUGINS.md` does not need to mention
this — the wall-clock budget is already documented; this is a
defense-in-depth strengthening of an existing guarantee.

## 9. Out of scope

- The `luaL_error` longjmp itself remains catchable by Lua-level
  `pcall`. We do not change Lua's error model. The fix is purely
  about throughput: how many cycles a malicious plugin can
  extract from each catch.
- Changing `kPcallBudgetMs` from 1500 ms. Tunable separately if
  user-facing handlers need more.
- Removing the `pcall` primitive from the sandbox entirely. Some
  plugins legitimately use `pcall` for graceful degradation
  (e.g. optional API feature-detection). Removing it is heavier
  than this defense in depth.

---

**Anchor:** roadmap entry `[ANTS-1332]` under § 0.7.92 Tier 1.
