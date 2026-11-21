# ANTS-1286 — Audit tool-detection cache (session-scoped)

**Status:** 📋 → (in implementation 2026-05-14)
**Kind:** optimize
**Source:** indie-review-2026-05-13
**Pairs with:** ANTS-1283 (`session_memory` KV — referenced as the
cross-process backing-store option, not used in v1).

## 1. Problem

Every `/audit` invocation re-runs `which <tool>` for every external
scanner the audit pipeline knows about. **Pre-ANTS-1286**,
`AuditDialog::toolExists` (`src/auditdialog.cpp:349`) spawned a
`QProcess`, ran `which`, and waited up to **3000 ms** per call. `grep -c "toolExists"
src/auditdialog.cpp` returns **27** lines — 1 is the function body's
own definition line, 1 is the function-internal forward, and the
remainder are call-sites in `setupChecks` and `addToolCheck`.
Practical call count per audit run: ~25 probes, with several tools
(cppcheck in particular) probed multiple times.

| Tool | Call count in setupChecks |
|---|---|
| cppcheck | **3** — lines 1128 ×2 (same expression: `toolExists("cppcheck"), toolExists("cppcheck"), nullptr`) + line 1150 |
| clazy-standalone | 1 |
| clang-tidy | 1 |
| trivy, jq, semgrep, ast-grep, osv-scanner, trufflehog, hadolint, checkov | 1 each |
| pylint, bandit, mypy, ruff | 1 each |
| npm, cargo, cargo-audit, go, govulncheck, golangci-lint | 1 each |
| shellcheck, luacheck, spotbugs | 1 each |
| `addToolCheck:391` (called per external tool from elsewhere) | re-probes once per tool registered |

Each probe is fast on cache-warm `which` (~1–5 ms on Linux when the
binary is in PATH; ~10–20 ms on a cold disk), but **~25 sequential
subprocess spawns + a 3 s timeout each in the worst case** is real
cost — and "cppcheck" is queried **three** times across the dialog
because the result wasn't memoised.

Indie-review-2026-05-13 flagged this as redundant: tool availability
does not change between consecutive audit runs in the same shell
session.

## 2. Surface

### 2.1 Engine (Qt6::Core-only)

`src/tooldetectionengine.{h,cpp}` joins `ants_core_lib`. Pure
functions, file-local static cache, no Qt6::Widgets dependency.

```cpp
namespace ToolDetectionEngine {
    // True if `tool` resolves via PATH (cached).
    bool exists(const QString &tool);

    // Resolved absolute path or empty string (cached).
    QString resolve(const QString &tool);

    // Test / diagnostic accessors.
    void clearCache();
    int  cacheSize();
    QString currentPathHash();
}
```

### 2.2 AuditDialog delegation

`AuditDialog::toolExists` collapses to a one-line forward:

```cpp
bool AuditDialog::toolExists(const QString &tool) {
    return ToolDetectionEngine::exists(tool);
}
```

Same signature, same return semantics, every existing call-site
keeps working. No call-site edits.

## 3. Invariants

| # | Invariant |
|---|---|
| INV-1 | Cache hits perform zero I/O — no subprocess, no `stat`. |
| INV-2 | PATH change invalidates the cache atomically: any call whose computed PATH-hash differs from the cached hash sees an empty cache. |
| INV-3 | Repeat probes of the same tool within a stable PATH return the cached result without re-resolving. A cached miss is re-probed once it is 30 s old, so a tool installed mid-session is found (ANTS-5110); a hit never expires. |
| INV-4 | Empty / null tool name returns `false` (or empty path) without caching. |
| INV-5 | Cache access is thread-safe via a single `QMutex` — IPC and GUI both call into it. |
| INV-6 | Cache is process-lifetime only. No disk persistence in v1; the ANTS-1283 backing-store hook is deferred (see § 7). |
| INV-7 | Resolution uses `QStandardPaths::findExecutable` — Qt-idiomatic, no subprocess, follows `PATH` semantics. No `QProcess` spawn. |
| INV-8 | `AuditDialog::toolExists` delegates entirely to `ToolDetectionEngine::exists`. The probe lives in exactly one place. |
| INV-9 | Cache size is bounded by the number of distinct tool names queried per process. Practical upper bound ≈ 30 entries (the audit pipeline's tool set); per-entry footprint ≈ 120–200 B (UTF-16 tool name ~30 B + UTF-16 resolved path ~50 B + `QHash` bucket overhead ~40–120 B). RAM budget ≤ ~10 KiB. |
| INV-10 | PATH-hash uses SHA-256 truncated to 16 hex chars (64 bits) — collision-negligible for the practical use case. |
| INV-11 | `clearCache()` resets cache + PATH-hash in a single locked operation. |

## 4. Implementation sketch

```cpp
// tooldetectionengine.cpp
namespace {
    QMutex g_mutex;
    QString g_pathHash;
    QHash<QString, QString> g_resolved;   // tool → path ("" = not found)

    QString hashCurrentPath() {
        // `qgetenv` returns the raw bytes (no codec round-trip).
        const QByteArray p = qgetenv("PATH");
        return QString::fromLatin1(
            QCryptographicHash::hash(p, QCryptographicHash::Sha256)
                .toHex().left(16));
    }
}

QString ToolDetectionEngine::resolve(const QString &tool) {
    if (tool.isEmpty()) return {};
    QMutexLocker lock(&g_mutex);
    const QString h = hashCurrentPath();
    if (h != g_pathHash) {
        g_pathHash = h;
        g_resolved.clear();
    }
    auto it = g_resolved.find(tool);
    if (it != g_resolved.end()) return it.value();
    const QString p = QStandardPaths::findExecutable(tool);
    g_resolved.insert(tool, p);
    return p;
}

bool ToolDetectionEngine::exists(const QString &tool) {
    return !resolve(tool).isEmpty();
}
```

## 5. Theme consistency

Mirrors the engine/MCP layering precedent (`SessionMemoryEngine`,
`RoadmapIndex`, `ColdEyesEngine`, `TokenUsageEngine`): pure-function
engine in `ants_core_lib`; GUI consumer is a thin delegating
forward. No new Widgets coupling.

## 6. RAM budget

≤ ~10 KiB process-lifetime (≈ 30 entries × 120–200 B each incl. `QHash`
bucket overhead, plus a 16-char PATH-hash). Bounded by the audit
pipeline's tool list which is itself bounded by `setupChecks`. The
cache shrinks to zero on PATH change. No eviction policy needed at
this scale.

## 7. Out of scope (v1)

- **Cross-process persistence via ANTS-1283.** The roadmap entry
  mentions it as an option ("or via ANTS-1283 session memory for
  cross-process persistence"). Skipped in v1 because:
  1. The dominant win is the 30× repeat-probe collapse within a
     single audit run.
  2. ANTS-1283's `session_memory` store is per-cwd; tool detection
     is **not** cwd-scoped, so backing it there would either store
     duplicate caches (one per cwd) or require a synthetic "global"
     cwd key. Either is awkward.
  3. If cross-process persistence becomes desired, the engine can
     gain a `loadFromSessionMemory()` / `saveToSessionMemory()`
     pair without changing call-sites — additive, non-breaking.
- **`stat`-based invalidation on the binary's mtime.** A removed or
  upgraded tool isn't detected until PATH changes. Acceptable
  trade-off: the cost of a stale cache entry is one wasted audit
  run (the tool reports "not found" or the upgrade's behaviour
  difference); the cost of a per-call `stat` is most of the
  optimisation. Restart of the terminal flushes the cache.
- **Async / threaded probe.** `QStandardPaths::findExecutable` is
  already non-blocking enough (μs-scale on warm cache, ms on cold).
  No event-loop pumping needed.

## 8. Test plan

`tests/features/tool_detection_engine/` (GTest, `ants_core_lib` only):

| # | Case | Asserts |
|---|---|---|
| TDE-1 | EmptyToolReturnsFalse | INV-4 — `exists("")` → false, cache unchanged |
| TDE-2 | KnownToolResolvesAndCaches | INV-3 — first call probes; cacheSize grows by 1; second call returns same value without growing the cache |
| TDE-3 | UnknownToolNegativeCaches | unknown name returns false, but cache still records the result (no re-probe on repeat) |
| TDE-4 | PathChangeInvalidates | INV-2 — `setenv("PATH", ...)` then call → cache reset; `currentPathHash` differs |
| TDE-5 | ResolveReturnsPath | `resolve("sh")` returns a non-empty path (sh is on every POSIX); `exists("sh")` is true |
| TDE-6 | ClearCacheResets | INV-11 — after clearCache, cacheSize == 0; next call repopulates |
| TDE-7 | RepeatProbesAreFree | INV-1 — 1000 repeat `exists("sh")` complete in < 5 ms (cache-hit budget) |
| TDE-9 | MissExpiresSoInstalledToolIsFound | INV-3 — with the miss TTL at 0, a tool created after a miss is found |

Source-grep test for delegation (joins existing audit test bundle):

| # | Case | Asserts |
|---|---|---|
| TDE-8 | AuditDialogToolExistsDelegates | INV-8 — `auditdialog.cpp` body of `toolExists` calls `ToolDetectionEngine::exists`; no `QProcess` / `"which"` left in the function |

## 9. Cold-eyes loop

Rows live in [`../reviews/ANTS-1286-audit-tool-detection-cache-session-loop-log.md`](../reviews/ANTS-1286-audit-tool-detection-cache-session-loop-log.md).
