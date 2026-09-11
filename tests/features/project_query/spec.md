# Feature: `project_query` — sandboxed server-side query verb (ANTS-2093)

Design spec: `docs/specs/ANTS-2093.md`. This is the feature-conformance
contract; the C++ test drives `LuaEngine::runQuery` /
`LuaEngine::projectQueryVerb` directly against `QTemporaryDir` fixtures and
source-scrapes the wiring.

## Invariants

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
- **INV-10** (marshal budget, ANTS-5069) — The §2.4 marshal charges each
  value's approximate serialised size against `resultCapBytes` as it walks
  the return value, and refuses with `result_too_large` as soon as the
  budget is spent. `marshalNodes` therefore stays bounded by the budget no
  matter how many paths a shared table has — a table referencing the same
  sub-table down every branch has few *distinct* values but many *paths*,
  and counting paths rather than distinct values is what let a small
  snippet exhaust memory before the byte-size check ever ran. *Tested:* a
  table sharing one sub-table down both branches at every level (so the
  full walk has far more paths than the VM holds distinct tables) refuses
  `result_too_large` with `marshalNodes` bounded to a small multiple of the
  cap; a flat array with no sharing at all, whose serialised size alone
  exceeds the cap, refuses the same way with `marshalNodes` bounded the
  same way; a result under the cap still succeeds, matches its expected
  JSON, and visits exactly as many values as it contains; a circular table
  (`t.self=t`) still refuses via the depth bound (INV-6), unaffected by the
  budget change.

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
