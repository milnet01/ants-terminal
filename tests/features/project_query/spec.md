# Feature: `project_query` — sandboxed server-side query verb (ANTS-2093)

Design spec: `docs/specs/ANTS-2093.md`. This is the feature-conformance
contract; the C++ test drives `LuaEngine::runQuery` /
`LuaEngine::projectQueryVerb` directly against `QTemporaryDir` fixtures and
source-scrapes the wiring.

## Invariants under test

- **INV-1 (no write surface)** — A query VM exposes only
  `project.read`/`list`/`root` + the read-only stdlib. `ants`, `os`, `io`,
  `require` are all absent (index to `nil`). *Tested:* a snippet returning
  `type(ants)`/`type(os)`/`type(io)`/`type(require)` yields `"nil"`; a
  snippet that calls `os.execute(...)` raises → `query_error`. Source-scrape:
  `registerQueryApi` installs `read`/`list`/`root` and no write callback.
- **INV-2 (FS confinement)** — `../../` traversal, an absolute out-of-root
  path, and an in-root symlink whose target escapes each raise
  (`query_error`); an in-root read succeeds.
- **INV-4 (budget)** — `while true do end` → `query_timeout`; an
  allocator-buster → `query_oom` (the refusal, not a nil/partial result).
- **INV-5 (no state bleed)** — call A sets `_G.x`; call B sees `x == nil`
  (fresh `lua_State` per call).
- **INV-6 (marshalling)** — one case per §2.4 row: nil→null, bool, integer,
  float, string, array-like table→array, string-keyed table→object, nested
  ok at 32 levels, a 33-level table → `query_error`, a circular table
  (`t.self=t`) → `query_error`, invalid UTF-8 → `query_error`, a
  function return → `query_error`.
- **INV-7 (output cap + list determinism)** — a result over the cap →
  `result_too_large` with no `result`; two `project.list` calls return
  byte-identical arrays.
- **INV-8 (refusal codes)** — `query_disabled` when the feature gate is off
  (checked before arg validation); `missing_field` when `code` is absent.
- **INV-9 (offload composition)** — `project_query` is in
  `isOffloadEligible` (source-scrape for the literal).

## Wiring (source-scrape — the verb glue isn't unit-testable standalone)

- `mainwindow.cpp` registers `project_query` (Required contract) under
  `#ifdef ANTS_LUA_PLUGINS`.
- `claudeintegration.cpp` registers the Required `CallerCwdContract` and
  lists the verb in the catalogue under `ANTS_LUA_PLUGINS`.
- `config.cpp` exposes `claudeMcpProjectQueryEnabled` (default true).

## Out of scope here

The pure-C-spin **detach** (worker held as a zombie, GUI resumes within
budget + grace) is an integration/manual check per the spec — the unit path
drives `runQuery` synchronously off the dispatch thread, and `query_timeout`
via the wall-clock hook is the deterministic unit proxy.
