# mcp_indie_review_tools — 5 new MCP tools wired to RemoteControl

Locks the wiring + envelope shape of the 5 `indie_review_*` MCP
tools (ANTS-1112).

## INVs

- INV-9 (per-name registration): each of the 5 tool names appears
  in `tools/list` with `inputSchema.type == "object"`. Source-grep
  against `claudeintegration.cpp` confirms 5 distinct entries.
- All 5 names appear in `mainwindow.cpp` `setupClaudeMcpProviders`
  as `registerToolProvider` calls (source-grep).
- All 5 cmdIndieReview* methods exist in `remotecontrol.h` (signature
  source-grep) and `remotecontrol.cpp` (definition source-grep).
- INV-10: an envelope round-trip — feed JSON
  `{lane1: report1, lane2: report2}` where both report `src/foo.cpp:42`
  and verify the response contains a finding with both lanes — is
  exercised by the existing `IndieReviewEngine::corroboratedFindings`
  test (no MCP roundtrip needed; the MCP handler is a thin wrapper
  delegating to the engine, so testing the engine + the wiring
  separately is sufficient).
