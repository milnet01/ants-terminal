# standalone_mcp_server — ANTS-4932

The contract is `docs/specs/ANTS-4932-standalone-mcp-server.md` § 3. This
directory holds the cases § 6.1 assigns to it; each test is named for its
invariant.

| Case | Invariant | How |
|---|---|---|
| `Inv1LinksNoGuiLibrary` | INV-1 | `readelf -d` on `ants-mcpd`: no `libQt6Gui`, `libQt6Widgets` or `libQt6DBus`. The same probe on `ants-terminal` finds all three, which is the positive control. |
| `Inv2OneEnumerationOfTheProjectScopedSet` | INV-2 | A recording `ToolSink` gets every name `mcp::registerProjectScopedVerbs` registers; none is in `mcp::terminalScopedVerbNames()`. Every `registerToolProvider("<name>"` in `src/mainwindow.cpp` is in that set. |
| `Inv3ToolsListAgreesWithTheTerminalPipeline` | INV-3 | `tools/list` from `ants-mcpd` against an in-process `ClaudeIntegration`: the same count, and the same `inputSchema` for each name. |
| `Inv5NoTerminalRefusesForwardedVerbsOnly` | INV-5 | `ANTS_MCP_SOCKET` names a path with no listener: `tab_list` refuses `no_terminal`, and `spec_lint` with a `caller_cwd` is `ok:true`. |
| `Inv6FallbackIsTheServerCwd` | INV-6 | Started in a fixture directory, `caller_cwd_info` with no `caller_cwd` reports `"source":"ServerCwd"` and that directory. |
| `Inv8ForwardNeverSynthesisesCallerCwd` | INV-8 | A stub terminal records the forwarded request. With no `caller_cwd` the arguments have none; with one, it arrives byte-identical. |
| `Inv9ConcurrentWritesFromBothHosts` | INV-9 | `ants-mcpd` and an in-process `RemoteControl` each append to one migrated project at once. Each host has at least one `ok:true`; the item count is the seeded item plus every `ok:true`; `PRAGMA integrity_check` is `ok`. |
| `Inv11UidChecksRefuseAnotherUid` | INV-11 | `mcpd::socketOwnedBy` and `mcpd::peerUidIs`, each called with this process's uid and with another. |
| `Inv13HoldIsSeenAcrossProcesses` | INV-13 | A migration hold taken in the test process makes `roadmap_log` through `ants-mcpd` refuse `roadmap_busy`; after release it is `ok:true`. |

INV-4 lives in `tests/features/mcp_dispatch_forward_completeness/` and INV-7
in `tests/features/mcp_tabspecific_contract/`, which own those contracts. INV-10
is the manual recipe in the parent spec's § 6.2. INV-12 is the existing
`McpMasterToggle.INV2_StartupGate` and `McpOrientation_Inv14.MainWindowExportsSocket`,
unmodified.

**Isolation.** The child inherits the bundle's `XDG_DATA_HOME` and
`XDG_CONFIG_HOME` sandbox (`tests/bundle_main_gui.cpp`), so it opens the test
process's store and never the user's. `ANTS_MCP_SOCKET` is always set, to a stub
or to a path with no listener, so no case forwards to a running terminal.
