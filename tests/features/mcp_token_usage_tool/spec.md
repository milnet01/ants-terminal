# mcp_token_usage_tool — feature contract

Source-grep contract test for the ANTS-1284 `token_usage` MCP tool
registration + RemoteControl wiring. Mirrors `mcp_plan_template_tool`.

## What this test guards

The end-to-end MCP wiring for `mcp__ants__token_usage`:

- **REG-1** — `"token_usage"` appears as a registered tool name in
  `src/claudeintegration.cpp`.
- **REG-2** — `registerToolProvider("token_usage", ...)` is present
  in `src/mainwindow.cpp` (the provider lambda dispatches to
  RemoteControl).
- **REG-3** — `cmdTokenUsage` is declared in
  `src/remotecontrol.h`.
- **REG-4** — `RemoteControl::cmdTokenUsage` is defined in
  `src/remotecontrol.cpp`.
- **REG-5** — region-scoped check: the `// ANTS-1284` registration
  block in `claudeintegration.cpp` declares
  `additionalProperties: false`.
- **REG-6** — region-scoped check: the block lists the optional
  args (`reset`, `include_zero`) the spec § 2.1 declares; the
  `required` array is empty.

## Region scan delimiter

The region is `[// ANTS-1284 …, next // ANTS- anchor OR
result["tools"] = tools;)` — block-local, generic-anchor pattern
identical to `mcp_plan_template_tool`. Future tool registrations
after this one will be automatically picked up as the new
end-of-region without code changes here.

## Bundle

`test_claude` — same bundle as `test_mcp_plan_template_tool.cpp`.
