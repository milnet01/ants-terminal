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

This standard is the ordered procedure so a new tool doesn't miss a
step; per-verb behaviour is
[mcp-behavioural-notes.md](mcp-behavioural-notes.md).

---

## Load-bearing contracts (quick reference)

The contracts a new tool composes with, and where each is stated. This
is a map, not a second copy: every line points at the checklist step or
sibling standard that OWNS the rule (relocated out of the always-loaded
`CLAUDE.md` preamble by ANTS-2088). Read the owner before building
against it — a restatement here is one more thing that can drift from
what it restates, which is what ANTS-4680 removed.

- **caller_cwd contract (ANTS-1404 / ANTS-1419)** — step 2.
- **caller_cwd resolution (ANTS-1401)**, **state routing (ANTS-1336 /
  ANTS-1435)** — step 3.
- **Path validation (ANTS-1295)** — step 4.
- **Response wrap (ANTS-1294)**, **raw reads (ANTS-2218)** — step 5.
- **A verb reporting ZERO must say what it looked at (ANTS-4374)** —
  step 5a.
- **Refusal codes** — step 6; the taxonomy itself is
  [mcp-error-codes.md](mcp-error-codes.md).
- **Writer/reader format parity (ANTS-2042 / ANTS-4134)** — step 6a.
- **`dry_run` preview (ANTS-2077 / 2136 / 2227)** — step 6b.
- **ETag 304 (ANTS-1499)** and its handler-local exception (ANTS-4108) —
  step 7.
- **`fields=` projection (ANTS-1720 / ANTS-4524)**, **compaction
  (ANTS-4673 / ANTS-4677)** — step 8.
- **Cache keying and relocation (ANTS-1439)** — step 9; the contract
  itself is [mcp-caches.md](mcp-caches.md).
- **Dispatch order of the shared transforms** — § Project overrides.

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
   m_claudeIntegration->registerToolProvider("<name>",
       ClaudeIntegration::CallerCwdContract::Required,   // step 2
       [this](const QJsonObject &args) -> QString { /* … */ });
   ```

2. **Choose the caller-cwd contract.** Pick one of `Required` /
   `Optional` / `TabSpecific` / `ProcessGlobal` (ANTS-1404 defined the
   enum + the static `ClaudeIntegration::callerCwdContractFor` table;
   ANTS-1419 made it the registration arg above). The two MUST agree —
   `registerToolProvider` compares the passed contract against the static
   table and, on drift, asserts via `Q_ASSERT_X` (debug) **and refuses the
   registration in every build config** (ANTS-1419 / ANTS-1834) — the tool
   goes missing, which is loud in its own right, rather than running
   mis-classified. So add/confirm the
   tool's entry in `callerCwdContractFor` too. The dispatch enforces `Required` (empty
   `caller_cwd` ⇒ `{ok:false, code:"caller_cwd_required"}`) before your
   lambda runs. The static table is still live: it is the accessor for
   inline-dispatched tools (`get_session_info`, `tool_info`), the source
   of the registration-time drift assert, and what tests / `tools/list`
   schema massaging query — so keep it in sync, don't treat it as
   vestigial.

   **Only `Required` is ENFORCED at dispatch.** `TabSpecific` and
   `ProcessGlobal` are classification-only (ANTS-1404 Phase 3a, and the
   enum's own comments say so), so a tab-scoped verb gates itself in its
   handler — `RcGate` is the shared helper for that, and step 3 notes it is
   used well beyond the state store. An unclassified tool defaults to
   `Optional`. Picking the word does not buy the check: the drift assert
   above compares your two declarations of it, and only `Required` becomes
   a call-time refusal.

3. **Resolve `caller_cwd` through the one helper.** If the tool is
   project-scoped, resolve the root via `ants::resolveCallerCwdRoot`
   (ANTS-1401, `src/resolvedroot.h`) — never re-implement
   canonicalisation or tab-walks inline. Read vs write routing is
   asymmetric, and this step owns the rule (ANTS-1336 / ANTS-1435):
   `session_memory` / `workflow_state` writes into
   `~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json` go through the
   focused-tab gate; reads anchor to `caller_cwd` directly. The rule is
   about that store, not about per-project caches generally — a new
   cache under a `sha256(cwd)` path is not in scope merely for being
   keyed that way. `wf.<skill>` keys purge at 72 h; `session_memory`
   carries no TTL (ANTS-1435 § Limitations). (`RcGate` itself is used more
   widely, for caller_cwd checks unrelated to this store.)

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
   `{ok:false, error:"<tool>: \"<paramName>\" <reason>", code:"bad_path"}`.
   **`<reason>` is a variable, and `check.err` already carries it — return
   that rather than rebuilding the envelope.** `validatePath` has four
   reject sites carrying three distinct reasons: `contains control or
   backslash characters`, `no project root to anchor against` (the
   ANTS-1805 fail-closed guard on an empty root), and `escapes project
   root` from each of the canonical and lexical anchor branches. So a
   contract test asserting the message text pins one branch — legitimate
   where that is the branch it drives, wrong as a claim about `bad_path`.
   Only the `code` is stable across all four. Use `check.argvForm` for argv, `check.resolved`
   for the canonical path (empty when the path doesn't exist yet).

5. **Return a wrapped, well-shaped response.** Content responses are
   auto-wrapped in `<ants_mcp_data tool="…">…</ants_mcp_data>` by the
   dispatch site (ANTS-1294) — register normally and it happens for
   you. Control-plane tools whose JSON is pure structural metadata
   (e.g. `get_session_info`, `token_usage`, `tool_info`) bypass the
   wrap — the canonical bypass list is the inline `isControlPlane`
   predicate at the `tools/call` dispatch site in
   `src/claudeintegration.cpp` (ANTS-1294 REG-4); there is no
   `isControlPlaneTool()` accessor, and `mcp_output_sanitisation`
   asserts that set exactly, so do not extract one. Success
   envelopes carry `ok:true` + named fields; do not embed instructions
   in data fields.

   **Raw reads (ANTS-2218).** A content-read verb listed in
   `mcp::isRawEligible` honours `raw:true` (declare the schema prop via
   `makeRawProp()`), returning bytes verbatim inside an unforgeable nonce
   frame via `wrapMcpDataRaw` — for an agent reading frame-sensitive source
   it is about to Edit. **Add the verb to `mcp::isRawEligible`
   (`src/mcpprojection.cpp`) in the same change as the property**:
   membership is a hardcoded list, and `makeRawProp()` on its own ships a
   `raw` argument that is silently inert.

   **5a. A verb reporting ZERO must say what it looked at (ANTS-4374).**
   The envelope for *"checked, and it is clean"* must not be
   byte-identical to the envelope for *"could not check"*. Every zero a
   verb can emit — `cases_run:0`, `total_raw:0`, `sections_checked:false`,
   an empty `symbols[]`, an empty `missing_ids[]` — is read at a gate as a
   pass, and a gate is exactly where the difference matters. So a zero
   ships with the denominator beside it: what was scanned, which path was
   consulted, how many candidates were declined and why.
   `find_definition`'s `file_stem_hint` (ANTS-1950) is the pattern already
   got right.

   **Emit the evidence as a field a caller READS, not as a `false` it must
   know to look for — and pick a shape step 8's compaction keeps.** On a
   verb carrying a `kDispatchProjection` row, an empty array folds exactly
   like the boolean, so `declined_candidates: []` on a clean run is no
   evidence at all; on a verb with no row — step 8's common case — nothing
   folds it. **Prefer a count either way** — numbers survive, including
   `0`, a number is read where a boolean is skipped, and the shape then
   survives a row added later. A `*_checked` boolean survives too, by
   suffix, but it is still a `false` the caller must know to look for, so
   it is the weaker of the two shapes rather than an exception to this
   rule. **And do not fold it into an existing failure signal**: a narrowed
   scope that legitimately matched nothing is not a partial run, and
   marking it one trades a silent wrong answer for a noisy one. Reached
   independently from three directions in one session; the instances are
   ANTS-4366, ANTS-4370, ANTS-4371, ANTS-4373, and the refusal side of
   ANTS-4350 / ANTS-4368 / ANTS-4387.

6. **Use the canonical refusal shape.** Every failure is
   `{ok:false, error:"<human readable>", code:"<taxonomy code>"}` with
   `code` drawn from [mcp-error-codes.md](mcp-error-codes.md). Reuse an
   existing code before minting a new one; if you mint one, add it to
   that doc in the same change.

   **6a. Writer/reader format parity (ANTS-2042).** When a
   discovery/reader verb recognises a target format that the paired
   *writer* can't yet produce, the writer MUST refuse with a
   format-aware code carrying a format-appropriate Edit-fallback `hint`
   (naming the discovered format's append shape, not a bare "use Edit") —
   never a generic absence code (`no_*`, `*_not_found`). **`format_mismatch`
   additionally carries the discovered `format` and the `path` to the
   recognised file; `unsupported_format` promises the `hint` alone**, per
   [mcp-error-codes.md](mcp-error-codes.md), which owns both envelopes. A generic absence code lies to the caller: their
   reader already saw the file, so "not found" sends them chasing a
   phantom-missing artifact instead of reaching for Edit. The rule
   applies whenever reader and writer discovery can diverge —
   `project_layout` discovering a `data/changelog.yaml` the
   Keep-a-Changelog writer can't append to, or `roadmap_query` parsing
   a `#### Pass N.M` heading roadmap that `roadmap_log`'s
   `create_section` can't splice — though `op:"append"` *does* render
   a pass block on that format (ANTS-2126 / ANTS-4117), which is why
   this is decided per op and not per verb.
   Instances: ANTS-2031 (`roadmap_log op:"create_section"` returns
   `format_mismatch` rather than a generic absence code on pass-headings —
   per **op**, not per verb; since ANTS-2126 a flip/annotate locator miss
   on that format is still `bullet_not_found`, and `amend_body` there
   returns `unsupported_format`),
   ANTS-2040 (changelog_log
   returns `format_mismatch` instead of `no_changelog` on YAML
   changelogs). The generic absence codes (`bullet_not_found`,
   `no_changelog`) are **not** retired — they remain correct for the
   genuinely-absent case (no roadmap bullet / no changelog of any
   kind). The *discovered-but-unwritable-format* branch belongs to
   `format_mismatch` **and `unsupported_format` together**, split between
   them by the ANTS-4134 test below. The `format_mismatch` code is defined in
   [mcp-error-codes.md](mcp-error-codes.md); reuse it rather than
   minting a per-verb variant. **Three codes are in play, and the choice
   is per *op*, never per verb:** `unrecognised_format` when the reader
   parsed nothing at all; otherwise choose between `unsupported_format`
   and `format_mismatch` by what **this op's** writer can produce.
   `roadmap_log` emits both on the *same* pass-headings file —
   `create_section` → `format_mismatch`, `amend_body` →
   `unsupported_format` — while `op:"append"` writes that format.

   **ANTS-4134 settled the boundary (2026-08-15) and
   [mcp-error-codes.md](mcp-error-codes.md) owns it: the discriminator is
   the op's OUTPUT ARTIFACT, not the verb.** `format_mismatch` where the
   format has no writer for the kind of thing this op emits;
   `unsupported_format` where the artifact is writable but this op's way of
   producing it is not. Both rows there now state it that way — derive the
   test from them.

   **6b. Every *mutating* verb takes `dry_run` (ANTS-2077 / 2136 /
   2227).** A bool, default false. When true the verb computes the
   would-be result envelope (carrying `dry_run:true`) and returns it
   *before* any disk write — the preview path must share the exact code
   that computes the write, so it can't drift. Declare the schema prop via
   the shared `makeDryRunProp()` factory (the pre-factory verbs
   roadmap_log / changelog_log / spec_log keep tailored copies). Supported:
   roadmap_log, changelog_log, spec_log, apply_edits, project_settings,
   feedback_log, audit_falsepos_log, indie_review_fold_in,
   cold_eyes_fold_in, debt_sweep_defer, roadmap_migrate, audit_dismiss,
   session_message, test_audit_fold_in, debt_sweep_apply_fix — the last
   three were this rule's outstanding tail until 2026-08-25, when all three
   were opened and found to declare the property and branch on it. The
   ROADMAP-fold-in verbs peek the would-be IDs via
   `RoadmapFoldIn::peekIds` (no `.roadmap-counter` bump) and skip
   `insertBlock`.

   **`roadmap_migrate` is a stated deviation from the "before any disk
   write" rule** (ANTS-3855 § 2.3.1): its preview opens the store, and on a
   machine with no store yet that *creates an empty schema-initialised
   one*. Required for the preview to be correct rather than merely
   convenient — `load()`'s counts are a diff against existing rows, so a
   throwaway store would report every item as an insert on a project that
   is already migrated, which is a confident wrong answer. The deviation is
   bounded: what a dry run may create is an empty schema (zero `project` /
   `section` / `item` / `element` / `history` rows), it writes no roadmap
   data, and it modifies an existing store no more than a rolled-back
   transaction does.

   **Out of scope:** read-only verbs (`get_*`, `*_query`, `find_*`,
   `read_*`), and the **session-state** writers `session_memory` and
   `workflow_state` — which mutate, and deliberately carry no `dry_run`.
   What the rule is about is durable **project or roadmap data**, where a
   wrong write is expensive to undo. Read it that way rather than as
   file-vs-store: `roadmap_migrate` and `session_message` (the mail verb,
   not the `session_memory` state writer) write the machine-global roadmap
   store and are both in scope, while the session-state pair write a
   regenerable cache and are not. The lists above are the live inventory;
   treat a verb on neither as unclassified rather than compliant.

   **File note for steps 7–8:** a tool's `inputSchema` lives in the
   `tools/list` builder in `src/claudeintegration.cpp` — a *different*
   file from the `registerToolProvider` call in step 1
   (`src/mainwindow.cpp`). The per-verb helpers those two steps call for —
   `makeEtagMatchProp()` and `makeCompactProp()` — are defined and used
   there. `fields` is injected by that same builder into every schema, so
   do not declare it.

7. **Opt into ETag for read tools (optional, recommended).** A
   read-mostly tool should support the "304 Not Modified" pattern
   (ANTS-1499): add it to `isEtagSupportedTool` and add a
   `makeEtagMatchProp()` line to its schema. The dispatcher injects the
   `etag` field (`applyEtagPattern` → `etagFor(responseText)`); the
   handler must **not** emit it — unless it owns its own 304 under the
   exception below. The dispatch short-circuits a matching
   `etag_match` to `{ok:true, unchanged:true, etag:"<same>"}`.

   **Exception — a timing-bearing envelope owns its own 304 (ANTS-4108).**
   The central etag hashes the whole response text, which is correct
   exactly when the response is a pure function of project state. A verb
   whose envelope carries per-run measurements is not: `spec_conformance`
   emits one measured-microsecond `observations[]` row per executed case,
   so a central hash differs on every run and the 304 could never fire.
   Such a verb stays **out** of `isEtagSupportedTool`, hashes the envelope
   *minus* the measured fields, and performs the short-circuit itself —
   see `RemoteControl::specConformanceBuildResponse`
   (`src/remotecontrol_docs.cpp`), § 2.3 of
   [ANTS-4108](../specs/ANTS-4108-spec-conformance-verb.md) — whose ETag
   deviation stands, while its second deviation, `fields=` declined, is
   superseded by ANTS-4524 — and the
   `Inv9EtagShortCircuitIsHandlerLocal` case in
   `tests/features/spec_conformance_verb/`, which asserts the absence.
   Per-run measurement in the envelope is the only condition that earns
   this; wanting a different hash is not one. A handler-local 304 still
   returns the same `{ok:true, unchanged:true, etag}` shape, and a refusal
   envelope is never short-circuited.

   One obligation comes with it, and a verb missing it is broken in a way
   no test of the handler can see. **It still declares an `etag_match`
   input property in its schema — inline, not via `makeEtagMatchProp()`,
   with a description naming the fields its etag excludes.** Only the
   `isEtagSupportedTool` entry is withheld, never the property. **What
   happens to an undeclared arg is the CLIENT's business, and neither
   outcome reaches your handler usefully**: a strict client refuses the call
   before the dispatcher is reached (measured on `spec_lint`, ANTS-4663), and
   one that sends it anyway marshals it against no declared type — ANTS-4624
   watched a `fields` array arrive as a string. The dispatcher itself neither
   strips nor rejects it: it passes the arguments through and names the key
   in `ignored_args`. Either way the 304 is silently unreachable. The shared
   factory is wrong here for a
   second reason: its description names no excluded fields, so a
   handler-local etag computed over a subset would ship undocumented.
   A second obligation stood here until 2026-08-25 and can no longer be met:
   **the verb no longer declines `fields=`, and cannot (ANTS-4524).** Projection
   is universal, so there is nothing to withhold; instead `projectFields`
   floors any envelope carrying `unchanged:true` and returns it whole. The
   hazard the old rule named is real and unchanged — central projection is
   skipped only on a *central* 304, so a handler-local one is invisible to
   the dispatcher and a caller passing `etag_match` and `fields` together
   would have `unchanged` projected out of its own 304. What moved is where
   the guard lives: in the shared transform, where it covers both kinds,
   rather than in each such verb's schema, where it was one omission from
   failing.

8. **Decide compaction — `fields=` needs no decision (ANTS-4524).**
   Response narrowing is universal: the dispatcher projects for every verb
   and injects the `fields` property into every schema, so a new verb gets
   it by existing. It narrows to named top-level fields and has two floors.
   A **304** (`unchanged:true`) is returned whole — so compose with ETag by
   listing `"etag"` in `fields`. A **refusal** is narrowed like any other
   envelope, then `ok` / `code` / `error` / `retry_after_ms` are
   re-inserted — so § 6a's `format`, `path` and `hint` survive only if the
   caller named them.

   What you DO answer is compaction, in `mcp::kDispatchProjection`
   (`src/mcpprojection.cpp`). A row there declares the `compact` argument
   (`mcp::isCompactArgTool` — add `makeCompactProp()` to the schema in the
   same change); its `defaultCompact` column says whether an ABSENT
   `compact` falls back to the default-ON `mcp::terseDefault()`. **Answer
   the second one on its own evidence.** These were one predicate with
   `fields=` until 2026-08-25, so a verb added for narrowing began
   compacting for callers who never asked, folding away `spec_lint`'s flag
   saying a check had not run (ANTS-4673). Say no to the default where a
   `false` or empty field in your envelope carries meaning a caller
   branches on.

   **No row at all is a fine answer**, and the common one: the verb then
   takes `fields=` like every other and is never compacted — by anyone,
   including a caller who passes `compact:true`, since the whole compaction
   step is gated on membership.

   **A row does not protect a meaning-bearing `false`; `defaultCompact:false`
   only spares the caller who did not ask.** An explicit `compact:true` on a
   verb in the table still folds it, and `mcp::isCompactDroppable` folds
   `null`, `false`, `""`, `[]` and `{}` alike — an empty array goes exactly
   as the boolean does. The one thing that survives regardless
   is `mcp::isProtectedCompactKey`: `ok`, `code`, `error`, `etag`, `found`,
   `unchanged`, and any key ending `_checked` (by suffix since ANTS-4677).
   So a field a caller branches on needs BOTH, not either: a
   `defaultCompact:false` row, which spares the caller who did not ask, and
   a surviving shape — the `_checked` suffix, or a count — for the caller
   who passes `compact:true` (step 5a).

9. **Follow the cache contract for any project-scoped cache.** If the
   tool reads/writes a per-project cache, key + relocate it per
   [mcp-caches.md](mcp-caches.md) (ANTS-1439): a path-keyed cache may
   go cold or orphan across a project move but MUST NEVER shadow (serve
   the old path's data under the new path).

10. **Schema hygiene.** `inputSchema.type == "object"`,
    `additionalProperties == false`, an explicit `required[]`, and a
    one-line `description` per property. Document `caller_cwd` with
    its step-2 contract word — all four are permissible, not just
    `Required` / `Optional`.

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

**A seam a test drives must be its own translation unit, not a helper
sharing a `.cpp` with the handler** (measured ANTS-3855, 2026-08-06). A
static archive is pulled in at *object* granularity, so a seam co-located
with its `RemoteControl::cmd*` handler drags `RemoteControl` →
`ants::resolveCallerCwdRoot` → `MainWindow` into everything that links
it — and `test_core` links `ants_core_lib` **alone**, with no
`ants_chrome_lib`. The one-TU shape failed `test_core`'s link with ~20
undefined `MainWindow` / `ClaudeIntegration` / `AuditEngine` symbols. So
the seam goes in its own `src/<verb>verb.{h,cpp}` under `ants_core_lib`
SOURCES (outside `ANTS_RC_SOURCES`), and the handler stays a thin TU in
`ANTS_RC_SOURCES_REL` — which also bumps the derived `TU N/M` head
markers in every sibling `remotecontrol*.cpp`, asserted by
`RcTuSplit.TuOrdinalMarkersAscend`. Appending the new TU **last** is the
cheap position: it renumbers no existing ordinal and moves no two-anchor
scrape window.

## Project overrides

The dispatch order is load-bearing: `mcp::withIgnoredArgs` →
idempotent-read cache → `applyEtagPattern` → `mcp::projectFields` →
`mcp::compactEnvelope` → `mcp::appendReadHints` → `mcp::tabularize` →
`mcp::offloadBody` → `mcp::withIgnoredArgs` **again** → `<ants_mcp_data>`
wrap. A new opt-in (a future projection-like transform) slots into that
chain; read the symbols named above and `ClaudeIntegration::wrapMcpData` for
the exact hook points before adding one.

**Three things constrain where yours goes.** `mcp::offloadBody` may replace the
whole body with a head+pointer envelope, which is why the advisory is applied
on both sides of it (ANTS-4626) — an annotation that must survive an
over-threshold response is re-applied after it, and on a cache hit only
there. And `mcp::compactEnvelope` may fold your `null` / `false` / `""` /
`[]` / `{}` away; **step 8 owns when that happens and this section does not
restate it** — three consecutive review loops corrected a restatement here
and each correction was wrong in a new way.

**The third is `raw:true`, and it is not a hop in the chain above — that is
why the list does not name one.** It is a flag read between `mcp::tabularize`
and the offload, and it does two things to the tail: the offload block is
guarded on `!rawRequested`, so a raw response is never spilled to a
head+pointer envelope, and the terminal wrap becomes `wrapMcpDataRaw` rather
than `wrapMcpData`. So a transform added after that point runs on bytes step 5
promises are verbatim. Read `mcp::isRawEligible(toolName) && args.raw` at the
dispatch site before placing yours.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 / Q2 / Q3 / Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 3 (same doc, independent, cold) | 2 / 3 / 3 / n-a | **First gate on this document**, triggered by ANTS-4129's edit to step 7. 8 verified, 0 dismissed, all fixed. Q4 is not asked of a standard. All three lanes independently led on the same two defects, both in the new step-7 exception and both the same shape — it sanctioned a handler-local 304 without stating what else comes with it: the verb must still **declare** an `etag_match` schema property (step 10's `additionalProperties:false` rejects the arg otherwise, so the 304 is unreachable while every handler-level test still passes), and it must **decline** `fields=`, which the linked ANTS-4108 § 2.3 records as the second forced deviation. The `fields=` defect was also found by the orchestrator while building the packet. Verified against the dispatch predicate, not inferred: `etagUnchanged` is only ever set inside the `isEtagSupportedTool` branch, so a handler-local 304 always falls through to `mcp::projectFields` and a caller sending `etag_match` + `fields` loses `unchanged` from its own 304. Pre-existing defects the same read surfaced: the `fields=` quick-reference claimed the projection set is "a subset of the ETag set" — it is not, `read_log` projects but does not 304 (13-name set vs 25-name set, compared element-wise); § 6a's `format_mismatch` MUST was unqualified where `mcp-error-codes.md` reserves the narrower `unsupported_format` for the per-op gap, which `roadmap_log` emits live (`amend_body` → `unsupported_format`, `create_section` → `format_mismatch`, both predicates opened); `dry_run` was a hard obligation stated only in the at-a-glance map and absent from the checklist the document calls "the ordered procedure so a new tool doesn't miss a step" — added as **6b** rather than a new step, because ANTS-4108 and ANTS-2021 cite steps 7 and 8 by number and renumbering would strand them; step 10 permitted two of step 2's four contract words; and three pointers sent authors to `CLAUDE.md` § Conventions for wrap mechanics, hook points and a state-routing note that section has never contained. **Resolved clean, so not in the tally:** step 4's `validatePath` signature and defaults, and step 2's `Q_ASSERT_X` contract-drift assertion — both checked against source, both accurate; the lanes could not check them only because the packet lacked those windows. |
| 2 | 2026-08-12 | 3 (same doc, independent, cold, same packet rebuilt) | 2 / 2 / 2 / n-a | 6 verified, 0 dismissed, all fixed. **Half were loop 1's own collateral**, which is the honest headline. All three lanes led on the same one: loop 1's § 6a rewrite asserted `format_mismatch` is for "a format the verb cannot produce on any op" — false, and refuted by the example beside it, since `roadmap_log` *does* write pass-headings on `append` (ANTS-2126 / ANTS-4117) while `create_section` still refuses `format_mismatch`. The rule was deleted rather than restated: `mcp-error-codes.md` owns that boundary and this document now defers to it, because a discriminator derived here was wrong twice. Two lanes also found the step-7 exception said to *declare* `etag_match` without saying **how** — the shipped exemplar builds it inline, and a conformer reaching for `makeEtagMatchProp()` ships a description that names no excluded fields for an etag computed over a subset. **Loop 1's stated mechanism for that obligation was itself wrong and is corrected here**: `additionalProperties:false` does **not** reject an undeclared arg — measured by calling a verb with one, the dispatcher drops it and reports `ignored_args` (`src/claudeintegration.cpp:11750-11764`), so the 304 is unreachable silently rather than loudly. Loop 1's row is left as written; this row is the correction. Pre-existing defects found: the `Instances:` line stated ANTS-2031's `format_mismatch` for the verb when it is per-op; and the step-1 template broke the line between `registerToolProvider(` and `"<name>",` while the test it prescribes scrapes for them adjacent — 87 registrations in `mainwindow.cpp` use the adjacent form and none the split one, so a conformer copying the template ships a registration its mandated test cannot see. Also scoped "tenant-hashed storage", a phrase used once and never defined. **Three further errors were caught inside this loop's own fix pass by executing each new claim**: the factory description does not "promise an etag over the whole response" (it names no scope at all), `RcGate` is used far beyond the two state verbs, and a positive discriminator was asserted where none could be grounded — all three corrected before the commit. **Filed, not fixed here:** `mcp-error-codes.md`'s `format_mismatch` row defines the code as "the whole verb cannot write the format" while its own example has five sibling ops writing that format — a contradiction in the document that owns the taxonomy, out of scope for this one. Resolved clean, not in the tally: `PathValidation::validatePath` — the qualifier is right, `src/pathvalidation.h:13` opens `namespace PathValidation`. |
| 3 (cap) | 2026-08-12 | 3 (same doc, independent, cold, packet rebuilt) | 5 / 2 / 1 / n-a | 8 verified, 0 dismissed, all 8 fixed. **Stopped at the `--max-loops` cap, NOT converged** — see the note below the table. Sharpest find: `isControlPlaneTool()` was named as the "canonical bypass list" and **exists nowhere in the tree** — the real one is an inline `isControlPlane` predicate at the `tools/call` dispatch site, and `mcp_output_sanitisation` scrapes for that literal, so an author extracting the accessor the doc named would have reddened the suite. That defect was **inside the packet's own "settled, do not re-confirm" list**, which claimed every unresolved identifier had been spot-checked; the lane overrode the instruction and said so, which is exactly the behaviour the brief asks for when a finding contradicts a stated fact. Also: "`Q_ASSERT_X`, i.e. debug builds" understated the enforcement — ANTS-1834 makes `registerToolProvider` **refuse the registration in every build config**, so drift makes the tool go missing rather than run mis-classified. § 6a's *trigger example* still said the bullet writer "can't splice" pass-headings, contradicting the per-op rule three lines below it. `dry_run`'s "every mutating verb" was false against its own lists — `audit_dismiss` ships one and was unlisted, while `session_memory` / `workflow_state` mutate and deliberately carry none; the rule is now scoped to verbs that write a **project file**. The quick-reference ETag bullet stated the opt-in unqualified against step 7's exception — a judgement call left open at loop 1 on the grounds that a cold loop should decide it, and it did. **Two findings were loop 2's own collateral**: deferring § 6a's boundary wholly to `mcp-error-codes.md` pointed at a gloss the shipped code contradicts, and deleting the discriminator left the MUST with no selection test at all — unfalsifiable at authoring. § 6a now states the per-op rule the code implements and names the contested part explicitly (ANTS-4134) instead of asserting or deferring. **One error was caught inside this loop's fix pass by executing the claim**: the drift log is `ANTS_LOG`, not the `qWarning` two lanes named — they reasoned from a *paraphrase in my packet*, and reading the source gave a stronger and different answer (refusal, not a warning). |
| 4 | 2026-08-25 | 3 (same doc, independent, cold) | 5 / 1 / 0 / n-a | 6 verified, 0 dismissed, all fixed. **New run**, triggered by ANTS-4524 making `fields=` universal and rewriting step 8, step 7's exception and the quick-reference bullet — `CLAUDE.md` rule 14, since a conformer now does something different. The 2026-08-12 run's cap was calm and its tail has since shipped. **Three lanes independently led on the same two defects, and the second is the run's most consequential.** § 6a still called the `format_mismatch` / `unsupported_format` boundary contested and pending ANTS-4134 — which shipped 2026-08-15; both rows in `mcp-error-codes.md` now state the artifact-not-verb discriminator, and the gloss § 6a quoted in the present tense sits only inside the `unsupported_format` row as the text that row records itself CORRECTING. So the document withheld a conformance test it forbade deriving, from the taxonomy that had settled it. Found at packet-build time as well as by every lane. And **§ Project overrides' dispatch chain omitted `mcp::compactEnvelope`**, which runs between `projectFields` and the wrap: that section exists to give hook points for a future projection-like transform, so an author placing one after `projectFields` ships a transform whose `false` / `[]` output compaction folds away — ANTS-4673's exact shape, three sections below the step that exists to prevent it. `mcp::withIgnoredArgs` was missing from the head of the chain too. **One finding was this session's own collateral, caught in the loop that created it**: the new `fields=` bullet said "a refusal and a 304 are floored and returned whole", and only the 304 is. A refusal is narrowed like anything else and then has `ok`/`code`/`error`/`retry_after_ms` re-inserted — so § 6a's own `format` / `path` / `hint` vanish unless the caller names them. Two lanes. **One lane alone found the sharpest reading of ANTS-4374's own rule**: it says to emit evidence as "an array or a count" rather than a `false`, and `isCompactDroppable` drops an empty array exactly like a `false`. The clean-run case — `declined_candidates: []` — is therefore no evidence at all on any verb that declares `compact`, which is the byte-identical envelope that bullet forbids. Numbers survive (including `0`), and a `_checked` suffix is protected by name; the rule now says so. **All three lanes raised the same open question and none filed it**: step 7 said an undeclared arg is "dropped by the dispatcher … so the handler never sees it". Resolved against the dispatch site rather than by reasoning — `it->second.handler(argsObj)` passes the arguments through unfiltered and `ignoredArgs` runs afterwards on the RESPONSE, so nothing strips it. Fixed rather than dismissed because the true mechanism is a different failure to debug: a strict client REFUSES the call before the dispatcher is reached (ANTS-4663), or sends it against no declared type (ANTS-4624 watched an array arrive as a string). This sentence's stated mechanism has now been wrong twice, loop 2 of the previous run having 'corrected' it to the claim just deleted. **Collateral outside the document, fixed there**: `ANTS-4108` § 2.3 still listed `fields=`-declined as a live deviation while step 7 says it cannot be declined — two lanes, and the spec is the half the standard cites. Resolved clean and NOT in the tally: whether `ignored_args` falsely reports `etag_match` on a handler-local-304 verb — it cannot, because that verb declares the property, so the key is in `known`. |
| 5 | 2026-08-25 | 3 (same doc, independent, cold, packet rebuilt) | 3 / 3 / 1 / n-a | 7 verified, 1 dismissed, all 7 fixed. **Three of the seven were loop 4's own fixes**, which is this document's fourth consecutive loop where that is the largest class — and all three landed in text loop 4 ADDED. The § Project overrides paragraph loop 4 wrote to warn about compaction was wrong twice over, found by all three lanes between them: table membership folds NOTHING on its own (an explicit `compact:true`, or `isDefaultCompactTool` plus `terseDefault()`, is what resolves it — `spec_lint` and `feedback_query` are in the table precisely so an unasked caller is NOT compacted, which is ANTS-4673's fix rather than its damage), and it ignored `isProtectedCompactKey`, which the same document had just been corrected to describe fifteen lines above. Loop 4's own chain fix was also short: `appendReadHints`, `tabularize` and `offloadBody` sit between compaction and the wrap, and a transform placed after the offload has no body left to act on. And loop 4 retired step 7's second obligation without touching the header that counts them, so 'a verb missing either is broken in a way no test can see' pointed at one obligation — two lanes, and a conformer hunting the second can only invent the per-verb `fields=` workaround ANTS-4524 deleted. **Two pre-existing, both Q2 against the sibling taxonomy.** § 6a stated ONE MUST-carry payload for a rule covering two codes, while `mcp-error-codes.md` says `unsupported_format` promises the `hint` alone and not `format` / `path` — so the same refusal had two documented envelopes; loop 4's settling of the boundary is what made the divergence reachable. And the quick-reference said a handler-local-304 verb 'uses neither', naming the predicate AND the factory, where step 7 requires the property declared inline — a conformer reading only the map ships a verb whose 304 is silently unreachable. **The one Q3 is the run's best finding and is older than either run**: the standard tells an author to pick one of four `CallerCwdContract` words and calls it the security contract, while only `Required` is enforced at dispatch. Someone writing a tab-scoped verb picks `TabSpecific`, believes the dispatch gates it, and ships a handler with no tab check — nothing refuses the registration and no handler test sees it. **One finding no lane could reach, and the packet was why**: the `dry_run` bullet's 'Not yet (ANTS-2227 tail)' named `test_audit_fold_in` and `debt_sweep_apply_fix`, and BOTH implement it — each declares the property via `makeDryRunProp()` and each handler branches on it, verified by opening all three call sites. `session_message` supports it too and appeared on neither list, under a sentence saying to treat such a verb as unclassified. Two lanes flagged the inventory as unverified rather than guessing, which is the correct behaviour and cost a round-trip that 1b should have spent instead. **Corrected in its own document, not carried into this one**: `mcp-error-codes.md`'s `format_mismatch` cell said ANTS-2126 left `create_section` 'the only refusing op' while its own next row has `amend_body` refusing on the same file. **Dismissed, recorded**: step 4 renders one of `validatePath`'s two reject reasons in its example envelope — true, and it changes nothing an author builds, since step 4 requires routing through the helper rather than hand-building the string. |
| 6 (cap) | 2026-08-25 | 3 (same doc, independent, cold, packet rebuilt + its three gaps closed) | 2 / 2 / 1 / n-a | 5 verified, 5 fixed. **Stopped at the cap (3 for a standard), NOT converged — and this one is a VIOLENT cap: 4 of the 5 landed on text THIS RUN wrote.** See the note below the table. **§ Project overrides was wrong in all three loops of this run, each time in a new way, and that is a fact about the paragraph rather than about any wording.** Loop 4 added it and omitted the protected keys; loop 5 corrected that and made table membership sound sufficient on its own; loop 6 found membership is the OUTER gate (a verb with no row is never compacted by anyone, including a caller passing `compact:true`) AND that the corrected list had dropped `null`. Two lanes, two angles, same paragraph. It was DELETED rather than corrected a fourth time: § Project overrides now states the dispatch ORDER, which is its job, and points at step 8 for when compaction fires. **The deletion moved an obligation, so step 8 absorbed what it was missing** (`documentation.md` § 2.1's consolidate-don't-reconcile): a row does NOT protect a meaning-bearing `false` — `defaultCompact:false` spares only the caller who did not ask, an explicit `compact:true` still folds it, and the sole unconditional protection is `isProtectedCompactKey`'s six names plus the `_checked` suffix. A conformer reading the old § Project overrides would have added a table row as armour and shipped exactly the ANTS-4673 class. **Two lanes found the chain still short after loop 5 lengthened it**: `mcp::withIgnoredArgs` is applied on BOTH sides of `offloadBody` (ANTS-4626), because an offload discards the pre-applied advisory and a cache hit skips the pre-apply entirely. The one paragraph raising the offload was the one that omitted it. **One pre-existing Q2 with real teeth**: `dry_run`'s scope test read "verbs that write a **project file**", while two verbs in its own Supported list — `roadmap_migrate` and `session_message` — write the machine-global roadmap store instead. An author of a store-writing verb applied the stated test, concluded the rule did not bind them, and was contradicted by the inventory three lines up. Re-scoped to durable project or roadmap DATA, which is the distinction the exclusions actually rest on. **The Q3 is the ANTS-4374 bullet arguing with the sentence loop 5 added to it**: it forbids evidence shaped as "a `false` it must know to look for" and then blessed a `*_checked` boolean. Both are permitted now, with the count named as preferred and the boolean as the weaker shape rather than an exception. Resolved clean, not in the tally: three lanes' packet-staleness questions (`mcp-error-codes.md`'s `create_section` clause, fixed in loop 5; the `makeFieldsProp()` call sites, which remain live and idempotent under the universal injection; ANTS-4663's attribution for the strict-client refusal, which its own source comment carries). |
| 7 | 2026-08-25 | 3 (same doc, independent, cold) | 2 / 1 / 0 / n-a | 3 verified, 1 dismissed, all 3 fixed. **New run**, re-armed by ANTS-4680's restructure: the quick-reference map became pointers and each bullet's content moved into the step that owns it. **The map came back clean** — two lanes independently checked every pointer against the step it names and found each rule stated where the pointer sends you. **All three lanes reported the same defect and all three proposed the same wrong fix**: that the dispatch chain omits a "raw framing" hop between `mcp::tabularize` and `mcp::offloadBody`. Reading the dispatch site settles it — `rawRequested` is a GATE, not a transform, so the chain list is correct and inserting the hop would have written a false ordering claim into the one list this document calls load-bearing. What IS true and was undocumented: `raw:true` suppresses the offload (`!rawRequested`) and swaps the terminal wrap for `wrapMcpDataRaw`, so "Two hops constrain where yours goes" was false — there are three. **One lane's open question is what caught it**, against two lanes' confident finding, and the cause was this run's own packet, which listed the gate in file order as though it were a hop. **The second Q1**: step 4 pinned the `bad_path` envelope's message to `escapes project root`, where `validatePath` has four reject sites carrying three reasons — an implementer copying the literal emits a wrong reason under a right code, and a step-4 re-read caught that the first fix's wording condemned a shipped test asserting that message for the branch it drives. **The Q2** was internal to § 6a: one sentence reserved the whole discovered-but-unwritable branch to `format_mismatch`, another eight lines below split it between two codes. **Dismissed**: a lane read the `unsupported_format` row as keying on a different test; it does not — its bolded boundary sentence carries the artifact test verbatim, and the lane saw only its opening clause because this packet elided the middle. Collateral: `docs/specs/ANTS-1295.md` § 6's rejection table predates ANTS-1805's fail-closed reason — a neighbouring spec, ledgered `out_of_scope` and not edited. |
| 8 | 2026-08-25 | 3 (same doc, independent, cold, packet rebuilt) | 1 / 3 / 1 / n-a | 5 verified, 5 fixed, 0 dismissed. **Stopped here by choice at loop 2 of a permitted 3** — the user needed the session wrapped; a third cold loop is owed by anyone wanting convergence, and this run did NOT reach it. **Three of the five landed on text loop 7 wrote**, which is a 60% self-collateral share and the same signature the 2026-08-25 run capped on — read it as a caution against a fourth loop rather than as a verdict on the document. **Two lanes independently found the sharpest one**: step 5a said an empty array folds like a boolean, stated unconditionally, while step 8 says a verb with NO table row — its common case — is never compacted by anyone. Two implementers would have shipped different envelope shapes for the same evidence field, and callers bind to the name. **The Q3** is the twin of that: step 5's raw paragraph, also loop 7's text, described `mcp::isRawEligible` membership passively and never told the author to add themselves to it, where both sibling steps state their registry edit outright — so a conformer ships `makeRawProp()` and gets a `raw` argument that is silently inert. **The Q1** was the one pre-existing defect: § 6a's ANTS-2031 instance still read as live behaviour, and no op returns `format_mismatch` *instead of* `bullet_not_found` any more — ANTS-2126 made flip/annotate write that format, leaving `format_mismatch` scoped to `create_section`, which takes no bullet locator. **Also fixed**: the steps 7–8 file note named `makeFieldsProp()`, which step 8 says is injected and must not be declared, and omitted `makeCompactProp()`, which is the helper those steps actually require; and step 8's closing sentence read as offering the table row and the surviving shape as alternatives when both are needed. **Carried, unsettled, raised by two lanes in each loop**: whether the ignored-args advisory derives its honoured set for `etag_match` from `isEtagSupportedTool`, which would make a handler-local-304 verb advertise the argument as ignored while honouring it. No window was given for `withIgnoredArgs`; it changes no rule in step 7 either way, so it was not filed as a finding. |

**Not converged at either cap, and the two caps are different animals.**

The 2026-08-12 run capped **calmly** — its findings were spread across the
document, and its one open item (§ 6a's `format_mismatch` boundary, pending
**ANTS-4134**) shipped on 2026-08-15 and was folded in at loop 4.

The 2026-08-25 run capped **violently**: 4 of loop 6's 5 findings landed on
text that run had itself written, and § Project overrides was corrected in
all three of its loops, each correction wrong in a new way. Under
`review-contract` § At the cap that ends this document's review as it
stands — **do not open a fresh run against this text.** A fourth loop would
start at loop 1 against a document whose last three loops were each repairing
the one before, and nothing in the evidence suggests it would stop. The bar
lapses with the text it was measured against: an authoring edit that changes
direction re-arms the gate normally.

**Size is not the signal** — the document is mid-pack among its siblings. Both
runs diagnosed the same cause and the second one proved it: this is a **hub**
that restates contracts owned by `mcp-error-codes.md`, `mcp-caches.md`,
`mcp-behavioural-notes.md` and the specs, and every restatement is an
independent chance to drift from its owner. Loop 6 acted on that for the first
time rather than only naming it — the § Project overrides restatement was
deleted and step 8 made complete, which is one instance of the durable fix.
**That tail was taken by ANTS-4680** (2026-08-25): the quick-reference map was
restatement by construction, every bullet in it a copy that could drift from
the step below or the sibling standard beside it. Each bullet's content moved
into the step that owns it and the map is pointers only. That is an authoring
edit that changes direction, so the bar above has lapsed with the text it was
measured against.
