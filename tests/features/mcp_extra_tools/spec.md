# mcp_extra_tools — ANTS-1244

Pins the wiring contract for the 3 new MCP tools (`roadmap_query`,
`tab_list`, `get_text`) added on top of the existing 6 (scrollback,
cwd, session_info, last_command, git_status, environment).

Source-grep harness — verifies the JSON-RPC tool registrations
exist, the dispatch cases route to the new providers, the provider
setters are declared, the provider members exist, the 3 cmd
handlers were promoted to public on `RemoteControl`, and the
MainWindow wires lambdas that delegate to `m_remoteControl`.

A behavioural drive of the live MCP socket would require standing
up a real `MainWindow` + `ClaudeIntegration` pair, which is out of
scope at this tier. The IPC verbs' own feature tests
(`remote_control_roadmap_query`, `remote_control_tab_list`,
`remote_control_get_text`) already cover the semantics of what
the providers return; this test only pins the MCP-side wiring so
a refactor that drops one of the 3 tools breaks CI.

## Invariants

| #  | Lane                | Statement |
|----|---------------------|-----------|
| 1  | mcp/protocol        | `tools/list` block in `claudeintegration.cpp` registers the 3 new tool names verbatim: `"roadmap_query"`, `"tab_list"`, `"get_text"`. |
| 2  | mcp/protocol        | `tools/call` dispatcher has 3 new `else if` clauses gating on each new tool name. |
| 3  | provider/decl       | `claudeintegration.h` declares the 3 new setters `setRoadmapQueryProvider`, `setTabListProvider`, `setGetTextProvider` with the right `std::function` signatures (`QString()`, `QString()`, `QString(int,int)`). |
| 4  | provider/storage    | `claudeintegration.h` carries the matching 3 private `std::function` member variables (`m_roadmapQueryProvider`, `m_tabListProvider`, `m_getTextProvider`). |
| 5  | remote/exposure     | `remotecontrol.h` lists `cmdRoadmapQuery`, `cmdTabList`, `cmdGetText` in a `public:` section (not under any `private:` block before them). |
| 6  | mainwindow/wiring   | `mainwindow.cpp`'s `setupClaudeMcpProviders` calls each of the 3 setters and the lambdas refer to `m_remoteControl`. |
| 7  | dispatcher/sentinel | The `get_text` dispatch case uses `isDouble()` to distinguish "tab argument absent" from "tab argument == 0", matching INV-9 of the spec. |

## Acceptance

Exit 0 = all 7 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. No per-feature CMakeLists.txt.

## Re-open conditions

- A future change removes one of the 3 tools (e.g. tools/list re-orders or drops).
- The `setRoadmapQueryProvider` / `setTabListProvider` / `setGetTextProvider` API is renamed; this test will need the rename mirrored.
- If a status-filter parameter is added to `roadmap_query` (the deferred follow-up in spec § 9), INV-1's tool descriptor changes and the dispatch case extracts a new arg — update INVs accordingly.
