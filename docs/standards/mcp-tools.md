<!-- ants-mcp-tools-standards: 1 -->
# Ants Terminal MCP-tool authoring standard

Project-local convention for adding a tool to the Ants MCP surface
(the `tools/call` provider registry that Claude Code talks to).

This is the **umbrella authoring checklist**. The deep contracts for
individual concerns live in their own docs and are referenced, not
restated:

- [mcp-error-codes.md](mcp-error-codes.md) — the `code` taxonomy on
  refusal envelopes (ANTS-1353).
- [mcp-caches.md](mcp-caches.md) — keying + relocation contract for any
  project-scoped cache (ANTS-1439).

The per-feature details (exact line ranges, the wrap mechanics) live
in `CLAUDE.md` § Conventions; this standard is the ordered procedure
so a new tool doesn't miss a step.

---

## When to add an MCP tool

Prefer surfacing existing terminal/IPC capability over inventing new
state. If a Kitty-style IPC verb already exists in
`src/remotecontrol.cpp`, wrap it; don't reimplement. A tool earns its
place when it saves a Claude session real tokens or round-trips
(read-bundling, on-disk-cache reads) — not as a thin alias.

## Authoring checklist

1. **Register the provider with its contract.** Add a
   `registerToolProvider` entry in `src/mainwindow.cpp` (grep
   `registerToolProvider` to find the existing block). Since ANTS-1419 the call is
   3-arg — the per-tool caller-cwd contract is the **2nd positional
   argument**, so the security contract is visible at the registration
   site, not a separate lookup:

   ```cpp
   m_claudeIntegration->registerToolProvider(
       "<name>",
       ClaudeIntegration::CallerCwdContract::Required,   // step 2
       [this](const QJsonObject &args) -> QString { /* … */ });
   ```

2. **Choose the caller-cwd contract.** Pick one of `Required` /
   `Optional` / `TabSpecific` / `ProcessGlobal` (ANTS-1404 defined the
   enum + the static `ClaudeIntegration::callerCwdContractFor` table;
   ANTS-1419 made it the registration arg above). The two MUST agree —
   `registerToolProvider` asserts (via `Q_ASSERT_X`, i.e. debug builds)
   that the passed contract matches the static table, so add/confirm the
   tool's entry in `callerCwdContractFor` too. The dispatch enforces `Required` (empty
   `caller_cwd` ⇒ `{ok:false, code:"caller_cwd_required"}`) before your
   lambda runs. The static table is still live: it is the accessor for
   inline-dispatched tools (`get_session_info`, `tool_info`), the source
   of the registration-time drift assert, and what tests / `tools/list`
   schema massaging query — so keep it in sync, don't treat it as
   vestigial.

3. **Resolve `caller_cwd` through the one helper.** If the tool is
   project-scoped, resolve the root via `ants::resolveCallerCwdRoot`
   (ANTS-1401, `src/resolvedroot.h`) — never re-implement
   canonicalisation or tab-walks inline. Read vs write routing is
   asymmetric (see the `session_memory` / `workflow_state` note in
   CLAUDE.md): writes on tenant-hashed storage go through the
   focused-tab gate; reads anchor to `caller_cwd` directly.

4. **Validate every path argument.** Any arg that is a path (`path`,
   `file`, `lane`, `reports_dir`, …) MUST go through
   `PathValidation::validatePath(raw, rootCanonical, toolName,
   paramName = "path", allowOutsideRoot = false)` (ANTS-1295,
   `src/pathvalidation.h`) before any filesystem op. Pass
   `allowOutsideRoot = true` only for the documented out-of-root cases
   (ANTS-1455, e.g. a `/tmp` `reports_dir`) — note this C++ helper param
   is distinct from the MCP-facing JSON arg, which is
   `allow_outside_project` (mapped to the request's `allowOutsideProject`
   in `mainwindow.cpp`). Reject envelope:
   `{ok:false, error:"<tool>: \"<param>\" escapes project root",
   code:"bad_path"}`. Use `check.argvForm` for argv, `check.resolved`
   for the canonical path (empty when the path doesn't exist yet).

5. **Return a wrapped, well-shaped response.** Content responses are
   auto-wrapped in `<ants_mcp_data tool="…">…</ants_mcp_data>` by the
   dispatch site (ANTS-1294) — register normally and it happens for
   you. Control-plane tools whose JSON is pure structural metadata
   (e.g. `get_session_info`, `token_usage`, `tool_info`) bypass the
   wrap — canonical bypass list is `isControlPlaneTool()` in
   `src/claudeintegration.cpp` (see ANTS-1294 REG-4). Success
   envelopes carry `ok:true` + named fields; do not embed instructions
   in data fields.

6. **Use the canonical refusal shape.** Every failure is
   `{ok:false, error:"<human readable>", code:"<taxonomy code>"}` with
   `code` drawn from [mcp-error-codes.md](mcp-error-codes.md). Reuse an
   existing code before minting a new one; if you mint one, add it to
   that doc in the same change.

   **File note for steps 7–8:** a tool's `inputSchema` lives in the
   `tools/list` builder in `src/claudeintegration.cpp` — a *different*
   file from the `registerToolProvider` call in step 1
   (`src/mainwindow.cpp`). The `makeEtagMatchProp()` / `makeFieldsProp()`
   helpers are defined and used there.

7. **Opt into ETag for read tools (optional, recommended).** A
   read-mostly tool should support the "304 Not Modified" pattern
   (ANTS-1499): add it to `isEtagSupportedTool` and add a
   `makeEtagMatchProp()` line to its schema, and emit an `etag` field.
   The dispatch short-circuits a matching `etag_match` to
   `{ok:true, unchanged:true, etag:"<same>"}`.

8. **Opt into `fields=` projection for high-volume reads (optional).**
   A tool with a large payload should support response narrowing
   (ANTS-1720): add it to `mcp::isFieldProjectionTool` and a
   `makeFieldsProp()` line to its schema. Compose with ETag by listing
   `"etag"` in `fields`.

9. **Follow the cache contract for any project-scoped cache.** If the
   tool reads/writes a per-project cache, key + relocate it per
   [mcp-caches.md](mcp-caches.md) (ANTS-1439): a path-keyed cache may
   go cold or orphan across a project move but MUST NEVER shadow (serve
   the old path's data under the new path).

10. **Schema hygiene.** `inputSchema.type == "object"`,
    `additionalProperties == false`, an explicit `required[]`, and a
    one-line `description` per property. Document `caller_cwd` as
    Required/Optional matching step 2.

## Tests (required)

Mirror the registration-presence + schema-validity asserts the existing
MCP feature tests use (e.g.
`tests/features/mcp_session_memory/test_mcp_session_memory.cpp`):

- **Registration presence** — the provider registry
  (`ClaudeIntegration::m_toolProviders`) is private, so existing tests
  source-string-match the registration *call* in the `mainwindow.cpp`
  source text instead:
  `EXPECT_NE(src.find("registerToolProvider(\"<name>\""),
  std::string::npos)`.
- **`tools/list` schema validity** — assert the `tools[]` entry exists
  with `inputSchema.type == "object"` and
  `additionalProperties == false`.
- **Contract behaviour** — at minimum: a `Required` tool refuses empty
  `caller_cwd` with `code:"caller_cwd_required"`; a path arg outside
  the root refuses with `code:"bad_path"`; the happy path returns the
  documented fields.

Prefer driving the pure logic (resolver, projection, validation)
directly where a helper exists, so the test doesn't need a live tab.

## Project overrides

The dispatch order is load-bearing: idempotent-read cache →
`applyEtagPattern` → `mcp::projectFields` → `<ants_mcp_data>` wrap. A
new opt-in (a future projection-like transform) slots into that chain;
read CLAUDE.md § Conventions for the exact hook points before adding
one.
