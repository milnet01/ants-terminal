# mcp_provider_registry — ANTS-1253 conformance

Source spec: [docs/specs/ANTS-1253.md](../../../docs/specs/ANTS-1253.md).

Source-grep conformance harness — does NOT spin up the MCP server.
It locks the post-ANTS-1253 wiring contract that consolidated 12
per-tool `setXProvider`/`m_xProvider` setter+member pairs into a
single `registerToolProvider` + `m_toolProviders` registry.

## What this test asserts

1. **INV-5 enforcer.** `claudeintegration.h` carries **0**
   `std::function<...> m_\w+Provider;` member declarations (down
   from 12). The new `m_toolProviders` is `std::map`-typed and
   named without a `Provider` suffix, so it does not match the
   regex by design — the test catches accidental re-introduction
   of a per-tool member. Asserted by
   `grep -cE 'std::function<.*>\s+m_\w+Provider\b' src/claudeintegration.h`
   returning 0.

2. **No `setXProvider` setter decls remain.** Asserted by
   `grep -cE 'void set[A-Z][A-Za-z]*Provider\b' src/claudeintegration.h`
   returning 0 (was 12).

3. **Single registrar declared.** `claudeintegration.h` carries
   exactly 1 `registerToolProvider(const QString &name, ToolHandler …)`
   declaration plus the `using ToolHandler = std::function<QString(const QJsonObject &args)>;`
   type alias.

4. **Explicit `<map>` include.** `claudeintegration.h` carries
   `#include <map>` (it is not transitively pulled in by the Qt
   header set; verified 2026-05-13).

5. **Dispatcher collapsed.** `claudeintegration.cpp` carries 0
   `(else )?if (toolName == "X" && m_XProvider)` outward-delegate
   branches (was 12) and exactly 1
   `(else )?if (toolName == "get_session_info")` inline branch
   (the documented carve-out — INV-4 in the spec).

6. **Registry referenced from dispatch.** `claudeintegration.cpp`
   contains ≥ 2 references to `m_toolProviders` (one in the
   registrar body, ≥ 1 in the dispatcher's `find()` lookup).

7. **INV-8 schema-vs-registry binding.** Every tool name
   appearing in `claudeintegration.cpp`'s `tools/list` schema
   builder (matched by `<toolVar>["name"] = "<name>"`) other than
   `get_session_info` has a matching
   `m_claudeIntegration->registerToolProvider("<name>", …)` call
   in `mainwindow.cpp::setupClaudeMcpProviders`. Catches
   schema-side additions that forget caller-side registration.

8. **12 lambda registrations.** `mainwindow.cpp` carries exactly
   12 `->registerToolProvider("` call sites (matches the spec's
   §10 step 4 verifier).

9. **INV-3 / get_text isDouble() gate preserved.** The
   `registerToolProvider("get_text", …)` lambda body in
   `mainwindow.cpp` still uses `isDouble()` to gate `args.value("tab")`
   AND `args.value("lines")` extraction (ANTS-1244 INV-9 — `tab=0`
   distinct from "tab omitted").

10. **INV-4 carve-out preserved.** `claudeintegration.cpp` still
    has the inline `get_session_info` branch reading
    `m_state` / `m_currentTool` / `m_contextPercent` from
    `ClaudeIntegration`'s own private state (no provider lookup).
