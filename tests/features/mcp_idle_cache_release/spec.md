# Feature: idle MCP worker frees its parse caches (ANTS-5208)

## Contract

`RemoteControl` keeps a parse of the roadmap (bullets with their bodies,
the section slices) and of the changelog, each for at most
`kRoadmapCacheTtlMs`. The shared-lane worker fills them, so the shared lane
frees them when it runs dry.

1. **`RemoteControl::releaseIdleCaches()`** drops the roadmap bullets cache,
   the section cache and the changelog cache, and the calling thread's
   roadmap parse memo. A query after it answers as it did before it.
2. **`ClaudeIntegration::setSharedLaneIdleHook(hook)`** — the hook runs on
   the shared-lane worker after a shared-lane job, when no other shared-lane
   job is queued. A burst of queued jobs runs it once, after the last. A
   bulk-lane job never runs it.
3. **Both processes install it.** `ants-mcpd` (`src/mcpdmain.cpp`) and each
   terminal window (`src/mainwindow.cpp`) set the hook to their
   `RemoteControl`'s `releaseIdleCaches()`.

## Rationale

Nothing freed the caches between calls, so the last parse of a whole
roadmap stayed resident in every idle `ants-mcpd`. Calls from a Claude Code
session arrive seconds apart, past the cache's lifetime, so freeing it when
the lane is idle costs a re-parse that would have happened anyway.
