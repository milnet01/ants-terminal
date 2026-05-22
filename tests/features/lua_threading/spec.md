# Feature: Lua plugins run off the GUI thread (ANTS-1750)

Parent spec: `docs/specs/ANTS-1750.md`. This is the feature-conformance
contract for that design; it locks the observable behaviour + the
structural invariants a future change could silently break.

## Motivation

Plugin event handlers used to run synchronously on the GUI thread, and
the Lua watchdog can only interrupt the VM between bytecode instructions
— so a single long-running pure-C call (or just a long busy handler)
froze the entire terminal UI. ANTS-1750 moves each plugin VM onto its
own worker `QThread` and delivers events asynchronously, so the UI stays
responsive no matter what a plugin does.

## Behavioural invariants (runtime)

- **R1 (INV-2) — the GUI thread is not blocked by a busy handler.** With
  a plugin whose event handler runs a long (~hundreds of ms) busy loop,
  `PluginManager::fireEvent` must return to the caller (and the GUI event
  loop must regain control) within a small fraction of the handler's
  runtime. Measured: a main-thread `singleShot(0)` callback posted *before*
  `fireEvent` runs within ~100 ms of the pre-fire timestamp, even though
  the handler busies a worker for far longer. Pre-fix (synchronous on the
  GUI thread) this callback is delayed by the full handler runtime.

## Structural invariants (source)

These lock the design contract from `docs/specs/ANTS-1750.md` § 2. Each
fails on pre-fix source (none of the symbols exist yet).

- **S1 (INV-1) — worker affinity.** `PluginManager` moves each engine to
  a worker thread: `pluginmanager.cpp` contains `moveToThread(` and
  `LuaEngine` exposes a worker-side `dispatchEvent` slot.
- **S2 (INV-11) — parentless engine.** `loadPlugin` constructs the engine
  with `new LuaEngine(nullptr)` (a parented `QObject` cannot
  `moveToThread`).
- **S3 (INV-5) — GUI-set abort flag.** `LuaEngine` declares
  `std::atomic<bool> m_abortRequested` and `instructionHook` reads it.
- **S4 (INV-3 health) — execution-time health bracketing.** `LuaEngine`
  declares `eventStarted` and `eventCompleted` signals; `PluginManager`
  declares `dispatchTo`, `healthyEngines`, and a `healthTick` slot, plus
  an `m_zombies` detach list.
- **S5 (INV-7) — synchronous settings.get blocks the worker, not the
  GUI.** `PluginManager::wireEngine` uses `Qt::BlockingQueuedConnection`
  for the `settingsGetRequested` edge.
- **S6 (INV-12) — no `lua_close` on the GUI thread.** No GUI-thread code
  path calls `engine->shutdown()` (the `unloadAll` + load-failure
  synchronous `shutdown()` calls are gone); teardown destroys engines via
  `deleteLater` posted to the worker.
- **S7 (INV-10) — targeted dispatch off the GUI thread.** The `MainWindow`
  keybinding shortcut routes through `PluginManager::dispatchTo`, not a
  direct `engineFor(...)->fireEvent(...)` on the GUI thread.

## Out of scope

- Wiring the dormant `keypress`/`output`/`line`/`prompt` events and their
  veto contract — ANTS-1736.
- Full process isolation (SIGKILL-able plugin host) — ANTS-1795.

## Test

`tests/features/lua_threading/test_lua_threading.cpp`, in the `test_lua`
bundle. Label `features;fast`. Suite `LuaThreading`. Runtime checks drive
a real `PluginManager` + temp plugin dir; source checks grep
`SRC_LUAENGINE_*` / `SRC_PLUGINMANAGER_CPP_PATH` + the MainWindow source.
Verified to fail against pre-fix source before the fix is restored.
