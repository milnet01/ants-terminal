# Feature: a plugin may register a handler from inside a handler

## Contract

`LuaEngine::fireEvent(event, data)` dispatches the handlers registered
for `event` at the moment the dispatch STARTS. A handler is an ordinary
Lua function running under `lua_pcall`, so it may legally call
`ants.on(...)` — for the event being dispatched or for any other. Doing
so must be safe, and must not change what the in-flight dispatch runs.

Two rules, and the second is what makes the first testable:

1. **Memory safety.** `m_handlers` is a
   `QHash<PluginEvent, std::vector<int>>`. `ants.on` reaches
   `engine->m_handlers[event]`, which can rehash the QHash (a new event
   relocates every value) and `push_back` on the vector, which can
   reallocate it (the same event). Either invalidates a pointer or
   iterator held across the call. `fireEvent` must therefore not iterate
   storage owned by `m_handlers` while a handler is running.

2. **Snapshot semantics.** A handler registered during a dispatch runs
   from the NEXT dispatch onward, never the current one. This is the
   observable consequence of rule 1's fix (iterate a copy), and it is
   also the only sane contract: a handler that registers a handler for
   its own event would otherwise be able to extend its own dispatch
   without bound.

## Rationale

ANTS-4441 (cold sweep 2026-08-18). `fireEvent` range-iterated
`it.value()` — the live vector inside the QHash — with `lua_pcall` in
the loop body. The 64-handler-per-event cap bounds growth but not
reallocation, so a single legitimate `ants.on` call from a handler was
enough to dangle the range-for's cached `begin`/`end`. Nothing in
PLUGINS.md restricts registering from a handler, and `luaengine` is the
component SECURITY.md scopes as the sandbox.

## Invariants

**INV-1 — a handler added during a dispatch does not run in that
dispatch.** Register three handlers on `line`; the first registers
twenty more on `line` (forcing at least one vector reallocation) and one
on `theme_changed` (forcing a QHash insert). One `fireEvent(Line)` must
produce exactly the three original handlers' log lines and none of the
late ones.

**INV-2 — the added handlers run on the next dispatch.** A second
`fireEvent(Line)` runs 3 + 20 = 23 handlers.

**INV-3 — the dispatch survives the mutation.** Both dispatches complete
and `fireEvent` returns. Built under ASan (`--preset=debug`, and the
pre-push hook's sanitized leg) this invariant is what catches the
use-after-free directly; in a Release build INV-1 and INV-2 stand in for
it, since the freed buffer usually still reads back correctly.

## Scope

### In scope
- `LuaEngine::fireEvent` handler-list lifetime across `lua_pcall`.

### Out of scope
- The per-event handler cap (64) and its error message — ANTS-4441 notes
  the cap bounds growth, not reallocation; the cap itself is unchanged.
- Unprotected `lua_pushstring` before the `lua_pcall` frame — that is
  ANTS-4442, a separate defect on the adjacent lines.
