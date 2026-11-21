# ANTS-1283 — `session_memory` MCP key-value store — review loop log

Record for [`../specs/ANTS-1283-session-memory-mcp-key-value.md`](../specs/ANTS-1283-session-memory-mcp-key-value.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

(loops appended below — pre-ship gating per
`feedback_cold_eyes_review_loop_for_specs`.)

### Loop 1 — 2026-05-14 (post-draft self-review)

- ✅ Surface matches roadmap spec (per-cwd, get/set/list ops, 100 KiB cap).
  Added explicit `delete` op (gap in the roadmap one-liner; sibling
  inverse of `set`).
- ✅ INVs cover the security perimeter (key regex, mode 0600/0700,
  value cap, total cap, atomic save).
- ✅ Echo hygiene matches ANTS-1319 precedent.
- ✅ Tests partition mirrors ANTS-1284/1290/1319 (engine bundle +
  MCP-wiring source-grep bundle).
- ✅ Memory budget stated (≤ 100 KiB in flight; on-disk ≤ 1 MiB lifetime).
- ✅ Build cost stated; aligns with sibling tools.
- ✅ Schema required-array decision (only `op` required at schema
  layer; conditional `key`/`value` enforcement in the handler) follows
  the `cold_eyes_brief` / `cold_eyes_cross_doc_diff` precedent —
  avoids JSON-Schema conditional-requirement hacks while still
  surfacing clean error codes.
- ✅ Cwd resolution reuses `resolveRootCanonical` (mirrors all other
  path-accepting MCP tools); `cwd` override path documented as a
  hash input not a permission check.
- ✅ Concurrent-writer policy explicit (last-writer-wins, no flock).
- ✅ Corrupt-store policy explicit (treated as empty, logged once).

CLEAN — proceed to Loop 2 against actual code paths.

### Loop 2 — 2026-05-14 (verify against current source)

- ✅ `resolveRootCanonical(m_main)` available at `remotecontrol.cpp:1217`.
- ✅ `QSaveFile` already used in `roadmapfoldin.cpp` (atomic-rename
  precedent).
- ✅ `QCryptographicHash::Sha256` available in Qt6::Core (no new dep).
- ✅ `~/.cache/` resolves via `QStandardPaths::writableLocation(
  QStandardPaths::GenericCacheLocation)` — Qt-canonical, not
  `getenv("HOME")` munging.
- ✅ `ants_core_lib` already aggregates engine sources in
  `CMakeLists.txt`; add one line for `sessionmemoryengine.cpp`.
- ✅ Test harness pattern (CMake `add_executable` + `add_test` with
  `features;fast` labels) precedented by `cold_eyes_engine` /
  `mcp_cold_eyes` (ANTS-1319) — no new infrastructure required.

CLEAN — proceed to implementation.
