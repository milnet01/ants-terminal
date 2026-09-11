# mcp_subsystem — ANTS-1251 conformance

Source spec: [docs/specs/ANTS-1251.md](../../../docs/specs/ANTS-1251.md).

This is a source-grep conformance harness — it does NOT spin up a
RemoteControl + QLocalServer + filesystem sandbox. It locks the
wiring contract between MCP `tools/list`, `tools/call`, the
`RemoteControl::cmdSubsystem` public method, the `subsystemmap`
parser/cache helper, the IPC dispatcher entry, and the MainWindow
provider lambda. Plus it asserts that the parser snapshot of the
current `CLAUDE.md` Module map clears the spec's ≥ 15 lane floor
(spec § 10 step 2).

## What this test asserts

1. `cmdSubsystem(const QJsonObject &req)` is declared public on
   `RemoteControl` alongside the ANTS-1244/1248/1249/1250 block.
2. `remotecontrol.cpp` and `subsystemmap.{h,cpp}` together carry
   ≥ 7 `// ANTS-1251-INV-N` anchors, one per spec invariant.
3. IPC dispatcher in `remotecontrol.cpp` routes `"subsystem"` →
   `cmdSubsystem`.
4. MCP `tools/list` registers a single `subsystem` entry with `op`
   enum `{map, files, recent_changes}` and `op` in `required[]`.
5. MCP `tools/call` dispatcher carries
   `toolName == "subsystem" && m_subsystemProvider`.
6. `claudeintegration.h` declares `setSubsystemProvider` plus
   `m_subsystemProvider` member with the
   `std::function<QString(const QJsonObject&)>` signature.
7. `mainwindow.cpp::setupClaudeMcpProviders` calls
   `setSubsystemProvider` and delegates to `cmdSubsystem`.
8. `cmdSubsystem` body contains the op-dispatch chain (string
   literals `"map"`, `"files"`, `"recent_changes"` appear) plus
   the `bad_op` and `unknown_lane` error codes.
9. `cmdSubsystem` composes `cmdGitState({op:"log", ...})` for the
   `recent_changes` op (INV-5).
10. `subsystemmap.h` exposes `Lane`, `parse`, and `cachedLanes`
    in the `SubsystemMap` namespace.
11. CMake wires `src/subsystemmap.cpp` into `ants_core_lib`.
12. The current `CLAUDE.md`'s `## Module map (src/)` parses to
    ≥ 15 unique lanes including `"vtparser"` (spec § 10 floor).

**ANTS-3414** — `op:map` honours an optional `name` substring filter.
The `subsystem` inputSchema declares a `name` property (so a passed
`name` is a real param, not flagged in `ignored_args`), and the
`op:map` branch of `cmdSubsystem` narrows the returned `lanes[]` to
lanes whose name contains the needle (`Qt::CaseInsensitive`) and echoes
`name` back. Empty/missing → the full map (back-compat). A needle
matching nothing yields an empty `lanes[]`, not an error — `op:map`
lists, it does not validate a lane the way `files` / `recent_changes`
do. Vestige feedback 2026-07-02.

## Why source-grep, not behavioural

A behavioural test would need to spawn `git`, fixture a Module map,
and round-trip JSON-RPC. This harness is faster (~ms) and locks the
*compile-time* shape of the wiring contract — it catches the common
regression "I edited cmdSubsystem and the dispatcher entry was lost"
without needing a full integration setup. Behavioural exercising
happens via manual relaunch + Claude-side tool-call once bytes land,
same shape as ANTS-1248 / 1249 / 1250.

## Floor (assertion 12)

The spec's § 10 step 2 floor of ≥ 15 lane entries is the canary:
if a future edit drops below 15 (e.g. a developer reorganises the
Module map into a different shape), this test flips red and forces a
parser update. The single named lane `"vtparser"` is also asserted as a
sanity check that the parser isn't returning garbage on a successful
parse.

## ANTS-1292 addendum — module map moved to `docs/subsystems.md`

The canonical module map moved out of CLAUDE.md (reloaded into every
Claude session) into `docs/subsystems.md`, served on demand via the
`subsystem` tool. See `docs/specs/ANTS-1292.md`. Assertions 12-14 lock it:
INV-12 parses the *resolved* source (≥ 15 lanes incl. `vtparser`),
INV-13 asserts `resolveSource()` prefers `docs/subsystems.md`, and INV-14
asserts the post-split CLAUDE.md parses to zero lanes (no duplicate
catalogue in the preamble).

## ANTS-5074 addendum — the cache is shared across threads with no lock

`SubsystemMap::cachedLanes()` keeps its parsed-lanes cache in a function-static
`QHash`, and `subsystemmap.h` used to say it is called from one thread. That
was never true once the same helper became reachable from more than one
place: the Independent Review dialog reaches it on the GUI thread through
`derivePartition`, the `subsystem` and `indie_review_partition` MCP verbs
reach it on the MCP worker thread, and the remote-control socket's
`subsystem` route reaches it on the GUI thread again. A `find` concurrent
with an `insert` that triggers a rehash is undefined behaviour on an
unguarded `QHash`. Fix: a function-static `QMutex` held around every cache
access (the `find` and the `insert`; the file read and parse may happen
outside it), and the header comment corrected. Three new `TEST` cases,
each tagged `ANTS-5074/…`:

- **Wiring (source-grep)** — the body of `cachedLanes()`, comments
  stripped, takes a lock (`QMutexLocker`, `std::lock_guard`, or
  `std::scoped_lock`) strictly before its first access to the shared
  cache. Red against the current tree: no lock construct appears anywhere
  in the body.
- **Mtime guard under the lock** — a lock must not change the cache's
  existing mtime-only invalidation contract (`ANTS-1251-INV-2`). Writing a
  file with new bytes but the SAME mtime still returns the previously
  cached lanes; bumping the mtime returns the new ones. Single-threaded
  and deterministic — a regression guard against a lock fix that
  accidentally reworks the invalidation logic it wraps, not a test of the
  concurrency defect itself.
- **Concurrency guard (behavioural)** — several threads call
  `cachedLanes()` on a handful of distinct temporary module-map files,
  many times each, while a dedicated thread keeps replacing one file's
  content (atomically, via write-temp-then-rename so a reader never sees a
  torn write) and bumping its mtime. Every non-empty result a reader gets
  must equal the lanes that some content the file legitimately held
  parses to — membership against the small closed set of variants that
  file was written with, not a single expected value, since a read can
  land on content that was current a moment ago — and the process must
  not crash.

  **This is a guard, not a strict pass/fail lock on the pre-fix code.** An
  unlocked `QHash` rehash race is undefined behaviour; undefined behaviour
  can run clean under light load exactly as easily as it can corrupt state
  or crash the process. A clean run here does not prove the fix is
  present; a mismatch or a crash does prove the race is still live. It is
  most reliably tripped under the ASan sanitizer build (this project's
  `debug` preset, or `tools/ci-parity.sh --full`), which turns the heap
  corruption from a racing `QHash` rehash into a deterministic abort with
  a diagnosis. Plain Release execution may pass by chance even against the
  unlocked code — this is the same posture `tests/features/file_content_cache`
  spec.md documents for its own concurrent-cache guard (`INV-4`), and for
  the same reason.
