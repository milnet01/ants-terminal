# ANTS-1247 — mcp_roadmap_status_filter feature test

## What it locks

The 10-INV contract for the `status` filter on `roadmap_query` MCP
tool + `cmdRoadmapQuery` IPC verb (per `docs/specs/ANTS-1247.md`):

1. **INV-1.** Back-compat: `cmdRoadmapQuery(QJsonObject{})` (zero-arg
   default) returns the full unfiltered set. Verified by header
   signature having `= {}` default + body branch reaching the
   pass-through path on empty `filter`.
2. **INV-2/3.** Active/shipped filter switch in source (status emoji
   compare against 📋/🚧/✅).
3. **INV-4.** Case-insensitive (`.toLower()` applied to status arg).
4. **INV-5.** Unknown status → `bad_status` error code; cache untouched.
5. **INV-6.** Cache-invariant preserved: the existing fresh check
   (`m_roadmapCacheMtimeMs` + TTL) is unchanged.
6. **INV-7.** Response includes `filter` field with canonical lowercase.
7. **INV-8.** MCP tools/list inputSchema declares `status` enum
   `["all","active","shipped"]`.
8. **INV-9.** MCP tools/call dispatch extracts `arguments.status`
   with `isString()` gate and forwards to provider.
9. **INV-10.** `count` reflects filtered size (the assignment line
   uses the filtered local, not `m_roadmapCacheBullets.size()`).
10. **INV-11.** Error-message hygiene: 64-byte cap + `< 0x20` →
    `'?'` replacement on attacker-supplied verbatim echo.

## Test shape

Source-grep, no GUI, no QTemporaryDir. Slurps:
- `src/remotecontrol.h` (signature, default param)
- the remotecontrol TUs (filter parse, switch, response shape, INV anchors)
- `src/claudeintegration.h` (provider signature widened)
- `src/claudeintegration.cpp` (inputSchema, dispatch extract)
- `src/mainwindow.cpp` (provider lambda)

Each INV is anchored to a `// ANTS-1247-INV-N` comment in source so
the test fails when a refactor moves the wiring away.
