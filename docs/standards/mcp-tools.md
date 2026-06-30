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
- [mcp-behavioural-notes.md](mcp-behavioural-notes.md) — per-verb
  behavioural reference (not authoring rules; ANTS-2088).

The per-feature details (exact line ranges, the wrap mechanics) live
in `CLAUDE.md` § Conventions; this standard is the ordered procedure
so a new tool doesn't miss a step.

---

## Load-bearing contracts (quick reference)

The contracts a new tool composes with, each with its spec. The
checklist below walks the full procedure; this is the at-a-glance map
(relocated out of the always-loaded `CLAUDE.md` preamble by ANTS-2088).

- **Response wrap (ANTS-1294).** `tools/call` replies are auto-wrapped
  in `<ants_mcp_data tool="…">…</ants_mcp_data>` by
  `ClaudeIntegration::wrapMcpData`. Register normally and the dispatch
  site wraps; control-plane tools (`get_session_info`, `token_usage`,
  `tool_info`) bypass. **Raw reads (ANTS-2218):** a content-read verb in
  `mcp::isRawEligible` honours `raw:true` (declare `makeRawProp()`),
  returning bytes verbatim in an unforgeable nonce frame via
  `wrapMcpDataRaw` — for agents reading frame-sensitive source to Edit it.
- **caller_cwd resolution (ANTS-1401).** Consume `caller_cwd` via
  `ants::resolveCallerCwdRoot` (`src/resolvedroot.h`) — never
  re-implement canonicalisation / tab-walks.
- **CallerCwdContract (ANTS-1404).** Classify each tool at
  `callerCwdContractFor` as Required / Optional / TabSpecific /
  ProcessGlobal; `Required` refuses empty `caller_cwd` with
  `code:"caller_cwd_required"`. Unclassified defaults to Optional.
- **Path validation (ANTS-1295).** Any path-typed arg routes through
  `PathValidation::validatePath` (`src/pathvalidation.h`) before any FS
  op; reject `code:"bad_path"`. Use `check.argvForm` for argv,
  `check.resolved` for the canonical path (empty if not-yet-existing).
- **ETag 304 (ANTS-1499).** Read tools opt in via `isEtagSupportedTool`
  + `makeEtagMatchProp()`; a matching `etag_match` short-circuits to
  `{ok, unchanged, etag}`.
- **`fields=` projection (ANTS-1720).** Opt in via
  `isFieldProjectionTool` + `makeFieldsProp()`; narrows to named
  top-level fields (a subset of the ETag set — list `"etag"` in `fields`
  to keep 304).
- **Refusal codes** follow [mcp-error-codes.md](mcp-error-codes.md);
  **caches** follow [mcp-caches.md](mcp-caches.md) (a path-keyed cache
  may go cold but must never *shadow*).
- **State routing (ANTS-1336 / ANTS-1435).** `session_memory` /
  `workflow_state` *writes* go through RcGate (focused-tab match);
  *reads* anchor to `caller_cwd`. `wf.<skill>` keys purge at 72 h;
  `session_memory` has no TTL. Storage
  `~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json`. See ANTS-1435
  §Limitations.
- **`dry_run` preview (ANTS-2077 / 2136 / 2227).** Every *mutating* verb
  takes a `dry_run` bool (default false). When true it computes the
  would-be result envelope (carrying `dry_run:true`) and returns it
  *before* any disk write — the preview path must share the exact code
  that computes the write so it can't drift. Declare the schema prop via
  the shared `makeDryRunProp()` factory (the pre-factory verbs roadmap_log
  / changelog_log / spec_log keep tailored copies). Supported:
  roadmap_log, changelog_log, spec_log, apply_edits, project_settings,
  feedback_log, audit_falsepos_log, indie_review_fold_in, cold_eyes_fold_in,
  debt_sweep_defer. The ROADMAP-fold-in verbs peek the would-be IDs via
  `RoadmapFoldIn::peekIds` (no `.roadmap-counter` bump) and skip `insertBlock`.
  Not yet (ANTS-2227 tail): test_audit_fold_in (struct-based, inline provider
  lambda) and debt_sweep_apply_fix (shell-exec — needs the fix script's own
  dry-run). Read-only verbs (`get_*`, `*_query`, `find_*`, `read_*`) are out
  of scope.

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

   **6a. Writer/reader format parity (ANTS-2042).** When a
   discovery/reader verb recognises a target format that the paired
   *writer* can't yet produce, the writer MUST refuse with
   `format_mismatch` — carrying the discovered `format`, the `path`
   to the recognised file, and a format-appropriate Edit-fallback
   `hint` (naming the discovered format's append shape, not a bare
   "use Edit") — never a generic absence code (`no_*`, `*_not_found`). A generic absence code lies to the caller: their
   reader already saw the file, so "not found" sends them chasing a
   phantom-missing artifact instead of reaching for Edit. The rule
   applies whenever reader and writer discovery can diverge —
   `project_layout` discovering a `data/changelog.yaml` the
   Keep-a-Changelog writer can't append to, or `roadmap_query` parsing
   a `#### Pass N.M` heading roadmap the bullet writer can't splice.
   Instances: ANTS-2031 (roadmap_log returns `format_mismatch` instead
   of `bullet_not_found` on pass-headings), ANTS-2040 (changelog_log
   returns `format_mismatch` instead of `no_changelog` on YAML
   changelogs). The generic absence codes (`bullet_not_found`,
   `no_changelog`) are **not** retired — they remain correct for the
   genuinely-absent case (no roadmap bullet / no changelog of any
   kind); `format_mismatch` is reserved for the *discovered-but-
   unwritable-format* branch. The `format_mismatch` code is defined in
   [mcp-error-codes.md](mcp-error-codes.md); reuse it rather than
   minting a per-verb variant. (If the reader *also* can't parse the
   file — zero recognised structure — that's `unrecognised_format`,
   not `format_mismatch`; the latter is for a *recognised* format the
   writer can't produce.)

   **File note for steps 7–8:** a tool's `inputSchema` lives in the
   `tools/list` builder in `src/claudeintegration.cpp` — a *different*
   file from the `registerToolProvider` call in step 1
   (`src/mainwindow.cpp`). The `makeEtagMatchProp()` / `makeFieldsProp()`
   helpers are defined and used there.

7. **Opt into ETag for read tools (optional, recommended).** A
   read-mostly tool should support the "304 Not Modified" pattern
   (ANTS-1499): add it to `isEtagSupportedTool` and add a
   `makeEtagMatchProp()` line to its schema. The dispatcher injects the
   `etag` field (`applyEtagPattern` → `etagFor(responseText)`); the
   handler must **not** emit it. The dispatch short-circuits a matching
   `etag_match` to `{ok:true, unchanged:true, etag:"<same>"}`.

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

11. **Keep the wire `description` short; move encyclopedic prose to
    `detail` (ANTS-2079).** Every tool's `description` ships in
    `tools/list` and is paid for once per session (plus per deferred
    schema load). When a `description` would exceed ~800 B of wire
    content, author a sibling `t["detail"] = QStringLiteral(...)` next to
    it: keep in `description` only the load-bearing contract surface
    (one *what + when* sentence, the op/selector set, the documented
    refusal `code`s, the `caller_cwd` contract word + required args), and
    move per-op/per-arg prose, `ANTS-NNNN` provenance, advisory notes,
    and full envelope listings into `detail`. The tools/list handler
    strips `detail` from the wire and appends a `tool_info {name:"…"}`
    pointer; `tool_info` serves `detail` on demand. Together
    `description` + `detail` MUST be a superset of the original — no
    documented code/op/arg dropped. Do NOT begin the short `description`
    with `[` (the runtime `[<kind>]` prefix loop adds it), and do NOT
    write the literal `props["` inside a descriptor comment (the
    `mcp_dispatch_forward_completeness` scraper mis-counts it as a schema
    prop).

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
