# Feature: Lua sandbox hardening — `string.dump`, `"t"` load mode, hook clear

## Problem

`LuaEngine::initialize()` builds a sandbox for plugin scripts by
removing dangerous globals (`os`, `io`, `load`/`loadfile`/`dofile`,
`debug`, `package`, `require`, `setmetatable`, `coroutine`, etc.)
and installing an instruction-count timeout hook. Three
defense-in-depth gaps persisted across 0.7.x:

### Gap 1 — `string.dump` was reachable

`string.dump(f)` returns the bytecode serialization of a Lua
function. Lua 5.4 has no bytecode verifier — the loader accepts
*any* sequence of bytes that starts with `\x1b` and parses as a
binary chunk, and crafted bytecode can corrupt Lua's internal
state and escape the sandbox. `LuaEngine::loadScript` already
rejects files starting with `\x1b`, but the primitive
`string.dump` was still present in the sandbox. The rejection
surface only catches file-level bytecode; any future C API added
to `ants.*` that wraps `luaL_loadbuffer`/`luaL_loadstring` with
plugin-supplied data would immediately give bytecode execution
back to attackers. Closing `string.dump` at the sandbox layer
(where the rule is adjacent to every other banned primitive) is
cheaper than auditing every future C API call for the same rule.

### Gap 2 — `luaL_dofile` accepted binary chunks by default

`luaL_dofile` forwards to `luaL_loadfile` which forwards to
`luaL_loadfilex(L, path, nullptr)`. The `mode=nullptr` default
means "text *or* binary" — the Lua loader happily parses a
`.lua` file whose first bytes are `\x1b Lua`. The pre-existing
0x1b-byte peek in `loadScript` catches this, but duplicating the
check at the Lua-loader level (by passing `"t"` explicitly) means
a future refactor that drops the peek still gets a rejection.

### Gap 3 — instruction-count hook retained across `lua_close`

`lua_close` runs every pending `__gc` metamethod in dependency
order. Metamethods can run arbitrary Lua code, and the
instruction-count hook observes every executed instruction. If
the hook fires mid-close and the hook body reads from
`LUA_REGISTRYINDEX`, it walks back into state that is being
actively finalized — a read-after-free in the worst case, or a
dereference of the dying engine pointer at best. Clearing the
hook before `lua_close` removes that window; the C-side cleanup
proceeds without any Lua-level observer.

## External anchors

- [Lua 5.4 Reference Manual §4.1 — Thread safety / no bytecode verifier](https://www.lua.org/manual/5.4/manual.html#4.1)
  — the official manual explicitly states Lua "does not check
  for well-formed binary chunks." `load`/`string.dump` round-trip
  is the documented unsafe primitive.
- [Lua 5.4 `luaL_loadfilex`](https://www.lua.org/manual/5.4/manual.html#luaL_loadfilex)
  — documents the `mode` parameter: `"b"` binary, `"t"` text,
  `"bt"` (default) both.
- [Lua 5.4 `lua_sethook`](https://www.lua.org/manual/5.4/manual.html#lua_sethook)
  — the hook is state-scoped and fires on any instruction;
  passing mask=0 clears it.
- 0.7.12 /indie-review Tier 2 findings:
  - *"Lua: strip `string.dump`, pass `"t"` mode to `luaL_loadfilex`.
    `luaengine.cpp:76, 206-213`. Defense-in-depth on bytecode
    rejection."*
  - *"Lua: clear hook before `lua_close` in `shutdown()`. Prevents
    UAF when finalizers run during destruction."*

## Contract

### Invariant 1 — `string.dump` is nil after `initialize()`

After `LuaEngine::initialize()` returns, evaluating
`string.dump` from a plugin script returns `nil` (not a C
function). A plugin attempting `string.dump(function() end)`
raises `attempt to call a nil value`.

### Invariant 2 — `loadScript` loads a valid text file

A plain `.lua` file with no leading `\x1b` loads, runs, and
observable side effects occur (tested by a script that sets a
registry-scoped flag via the plugin API or by a print intercept).

### Invariant 3 — `loadScript` rejects a file starting with `\x1b`

A file whose first byte is `\x1b` is rejected by `loadScript`
(return value `false`). This is the first-gate peek. We keep
testing it so the gate doesn't regress silently.

### Invariant 4 — `luaL_loadfilex` is called with `"t"` mode

Source-grep: `luaL_loadfilex` appears in `luaengine.cpp` with
the `"t"` mode argument, and the raw `luaL_dofile` call is gone.
Second gate for gap 2.

### Invariant 5 — hook cleared before `lua_close` in `shutdown`

Source-grep: `lua_sethook` with a `nullptr` callback appears in
`shutdown()` before the `lua_close` line. Closes gap 3.

### Invariant 6 — `string.dump` removal is scoped to the string table

Source-grep: the nil-set uses `lua_setfield(m_state, -2, "dump")`
(scoped to the already-loaded `string` table), not
`lua_pushnil` + `lua_setglobal("dump", ...)` which would blow
away an unrelated global. The former is correct; the latter
would be a silent bug.

## How this test anchors to reality

The test:

1. Constructs `LuaEngine`, calls `initialize()`.
2. Writes a tiny script to a tempfile:
   ```lua
   _G.ants_test_string_dump_type = type(string.dump)
   _G.ants_test_dump_errored = false
   local ok = pcall(function()
       local f = function() end
       return string.dump(f)
   end)
   _G.ants_test_dump_errored = not ok
   ```
3. Calls `loadScript(path)`. Retrieves the two globals via
   `lua_getglobal` + `lua_toboolean`/`lua_tostring`.
4. Asserts `ants_test_string_dump_type == "nil"` (I1a) and
   `ants_test_dump_errored == true` (I1b).
5. Writes a second script that starts with `\x1b Lua\0` (valid
   binary header bytes) and calls `loadScript`; expects `false`
   return (I3).
6. Source-greps `luaengine.cpp` for `luaL_loadfilex(..., "t")`
   (I4), the `lua_sethook(m_state, nullptr, 0, 0)` before
   `lua_close` (I5), and the string-table-scoped nil-set (I6).

## Regression history

- **Introduced:** 0.6.0-ish when the Lua sandbox landed. The
  `dangerous[]` global-nil loop was comprehensive for top-level
  globals but missed table members like `string.dump`. The
  bytecode-rejection peek landed in 0.6.9 (post-`lua_bundle`
  security review) but was scoped to file load, not to the
  loader mode.
- **Flagged:** 2026-04-23 /indie-review Tier 2 Lua reviewer.
- **Fixed:** 0.7.21 — `string.dump` removed at sandbox-init
  time; `loadScript` forces `"t"` mode via `luaL_loadfilex`;
  `shutdown()` clears the instruction-count hook before
  `lua_close`.
