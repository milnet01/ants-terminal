# Feature: a plugin at its heap budget cannot abort the terminal

**Status:** shipped (ANTS-4442)

PLUGINS.md documents the per-plugin heap budget as a state a plugin *lives
in* — allocations return `NULL`, the plugin is not unloaded — and states the
invariant "The terminal will never crash because of your plugin."

`LuaEngine::fireEvent` pushed the handler and its argument before entering
`lua_pcall`. `lua_pushstring` allocates; at the budget the capped allocator
returns `NULL`; Lua raises a memory error with no protected frame to catch
it; `luaPanicHandler` logs and returns, and its own comment concedes that
Lua treats a non-jump return from `atpanic` as fatal — so the process
aborts. That is an *expected* condition reaching a fatal path.

The panic handler's comment assumed no unprotected allocation site existed
("today: rare"). These two were exactly that.

`runQuery` is not affected: it loads through `luaL_loadbufferx`, which
returns `LUA_ERRMEM` rather than raising.

## Invariants

**INV-1 — dispatch at the heap budget does not abort.** Behavioural. With
the plugin holding most of the budget, firing an event whose payload cannot
be allocated returns normally. Reaching the next statement *is* the
assertion: pre-fix the process is gone. Measured against the pre-fix code —
SIGABRT from `lua_pushstring` called by `fireEvent`, which ctest reports as
"Subprocess aborted". Each ctest entry is its own process, so that red
costs no sibling test.

**INV-2 — the failure is reported, not swallowed.** Behavioural. The failed
dispatch emits a plugin-error message, so a handler that silently never ran
is distinguishable from one that ran and did nothing.

**INV-3 — the engine is still usable afterwards.** Behavioural. A later
event with a payload that fits still reaches the handler. This is what
proves the Lua stack was left balanced by the failure path rather than one
value deep.

**INV-4 — ordinary dispatch is unchanged.** Behavioural. A handler still
receives its payload intact, byte count included. The fix moves both pushes
into a protected call, so this guards the refactor rather than the defect.

**INV-5 — the pushes happen inside a protected call.** Source-grep against
`src/luaengine.cpp`: `fireEvent`'s body must reach `lua_pcall` before it
pushes the handler, and must secure stack space with `lua_checkstack`, whose
failure is reported by return value rather than raised.
