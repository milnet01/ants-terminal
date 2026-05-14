# tool_detection_engine — feature-conformance spec

**Owner:** ANTS-1286 (`docs/specs/ANTS-1286.md`)
**Subject:** Process-lifetime PATH-keyed tool-availability cache.

GTest cases against the engine in isolation (no GUI, no QProcess).

## Cases (TDE-1..TDE-8)

| # | Case | Asserts |
|---|---|---|
| TDE-1 | EmptyToolReturnsFalse | INV-4 — `exists("")` → false; cache untouched |
| TDE-2 | KnownToolResolvesAndCaches | INV-3 — first call probes, cacheSize grows; second call hits cache without growth |
| TDE-3 | UnknownToolNegativeCaches | unknown tool returns false and is recorded; repeat call does not grow cache |
| TDE-4 | PathChangeInvalidates | INV-2 — mutating `$PATH` resets the cache on next call |
| TDE-5 | ResolveReturnsPath | `resolve("sh")` returns non-empty path; `exists("sh")` is true |
| TDE-6 | ClearCacheResets | INV-11 — `clearCache()` → cacheSize == 0; next call repopulates |
| TDE-7 | RepeatProbesAreFree | INV-1 — 1000 repeat `exists("sh")` complete in < 5 ms |
| TDE-8 | AuditDialogToolExistsDelegates | source-grep INV-8 — `auditdialog.cpp`'s `toolExists` body calls `ToolDetectionEngine::exists` and contains no `QProcess` |

## Build wiring

`tests/features/tool_detection_engine/test_tool_detection_engine.cpp`
joins the `test_audit` bundle alongside
`tests/features/session_memory_engine/test_session_memory_engine.cpp`.
