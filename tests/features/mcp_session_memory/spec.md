# mcp_session_memory — feature-conformance spec

**Owner:** ANTS-1283 (`docs/specs/ANTS-1283.md`)
**Subject:** MCP wiring layer (RemoteControl + ClaudeIntegration +
MainWindow) for the `session_memory` tool.

Pure source-grep tests against the as-shipped source files — no
process spawn, no Qt event loop. Mirror to `mcp_cold_eyes`'s
pattern: anchor on `// ANTS-1283` comments + walk to the next
`tools.append(t);` for block-end.

## Cases (REG-1..REG-6)

| # | Case | Asserts |
|---|---|---|
| REG-1 | `session_memory` registered under ANTS-1283 anchor | tool name + comment anchor both present in `claudeintegration.cpp` |
| REG-2 | Schema `required:["op"]` (INV-9) | only `op` is schema-required; key/value enforcement is handler-side |
| REG-3 | `cmdSessionMemory` extracts every arg | `op`, `key`, `value`, `cwd` all `req.value(...)`d |
| REG-4 | Error codes complete | `bad_op`, `bad_key`, `bad_value`, `no_project` strings all present |
| REG-5 | Provider lambda registered in `mainwindow.cpp` | `registerToolProvider("session_memory", ...)` + `cmdSessionMemory(args)` forwarded |
| REG-6 | Handler delegates to engine | `cmdSessionMemory` body calls `SessionMemoryEngine::execute` |

## Build wiring

`tests/features/mcp_session_memory/test_mcp_session_memory.cpp` joins
the `test_claude` bundle alongside
`tests/features/mcp_cold_eyes/test_mcp_cold_eyes.cpp`.
