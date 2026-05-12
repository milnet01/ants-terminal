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
if a future CLAUDE.md edit drops below 15 (e.g. a developer
reorganises the Module map into a different shape), this test
flips red and forces a parser update. The single named lane
`"vtparser"` is also asserted as a sanity check that the parser
isn't returning garbage on a successful parse.
