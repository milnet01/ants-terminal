# mcp_plan_template_tool — feature contract

Source-grep contract test for the ANTS-1290 `plan_template` MCP tool
registration + RemoteControl wiring. Mirrors `mcp_verify_changes_tool`.

## What this test guards

The end-to-end MCP wiring for `mcp__ants__plan_template`:

- **REG-1** — `"plan_template"` appears as a registered tool name in
  `src/claudeintegration.cpp`.
- **REG-2** — `registerToolProvider("plan_template", ...)` is present
  in `src/mainwindow.cpp` (the provider lambda dispatches to
  RemoteControl).
- **REG-3** — `cmdPlanTemplate` is declared in `src/remotecontrol.h`.
- **REG-4** — `RemoteControl::cmdPlanTemplate` is defined in
  `src/remotecontrol.cpp`.
- **REG-5** — region-scoped check: the `// ANTS-1290` registration
  block in `claudeintegration.cpp` declares
  `additionalProperties: false`.
- **REG-6** — region-scoped check: the block lists the required arg
  (`feature_name`) and every optional arg the spec § 2.1 declares.

## Region scan delimiter

The region is `[// ANTS-1290 …, result["tools"] = tools;)` — block-
local. Future tool registrations after this one should insert their
own `// ANTS-NNNN` anchor; this test will then need its `block_end`
tightened to find the next anchor first (same pattern as the
verify_changes test was tightened in the ANTS-1290 commit).

## Bundle

`test_claude` — same bundle as `test_mcp_verify_changes_tool.cpp`.
