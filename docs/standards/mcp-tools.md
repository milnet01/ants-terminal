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
- **A verb reporting ZERO must say what it looked at (ANTS-4374).** The
  envelope for *"checked, and it is clean"* must not be byte-identical to
  the envelope for *"could not check"*. Every zero a verb can emit —
  `cases_run:0`, `total_raw:0`, `sections_checked:false`, an empty
  `symbols[]`, an empty `missing_ids[]` — is read at a gate as a pass, and
  a gate is exactly where the difference matters. So a zero ships with the
  denominator beside it: what was scanned, which path was consulted, how
  many candidates were declined and why. `find_definition`'s
  `file_stem_hint` (ANTS-1950) is the pattern already got right.
  **Emit the evidence as a field a caller READS, not as a `false` it must
  know to look for** — an array or a count is noticed, a boolean is not,
  and `compact:true` drops a `false` entirely. **And do not fold it into an
  existing failure signal**: a narrowed scope that legitimately matched
  nothing is not a partial run, and marking it one trades a silent wrong
  answer for a noisy one. Reached independently from three directions in
  one session; the instances are ANTS-4366, ANTS-4370, ANTS-4371,
  ANTS-4373, and the refusal side of ANTS-4350 / ANTS-4368 / ANTS-4387.
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
  `{ok, unchanged, etag}`. **Except an envelope carrying per-run
  measurements, which owns its own 304 and uses neither — step 7.**
- **`fields=` projection (ANTS-1720).** Opt in via
  `isFieldProjectionTool` + `makeFieldsProp()`; narrows to named
  top-level fields. Independent of the ETag opt-in, not a subset of it
  (`read_log` projects but does not 304); where a verb has both, list
  `"etag"` in `fields` to keep the 304.
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
  debt_sweep_defer, roadmap_migrate, audit_dismiss. **`roadmap_migrate` is a stated
  deviation from the "before any disk write" rule** (ANTS-3855 § 2.3.1): its
  preview opens the store, and on a machine with no store yet that *creates an
  empty schema-initialised one*. Required for the preview to be correct rather
  than merely convenient — `load()`'s counts are a diff against existing rows,
  so a throwaway store would report every item as an insert on a project that
  is already migrated, which is a confident wrong answer. The deviation is
  bounded: what a dry run may create is an empty schema (zero `project` /
  `section` / `item` / `element` / `history` rows), it writes no roadmap data,
  and it modifies an existing store no more than a rolled-back transaction
  does. The ROADMAP-fold-in verbs peek the would-be IDs via
  `RoadmapFoldIn::peekIds` (no `.roadmap-counter` bump) and skip `insertBlock`.
  Not yet (ANTS-2227 tail): test_audit_fold_in (struct-based, inline provider
  lambda) and debt_sweep_apply_fix (shell-exec — needs the fix script's own
  dry-run). Read-only verbs (`get_*`, `*_query`, `find_*`, `read_*`) are out
  of scope, as are the **session-state** writers `session_memory` and
  `workflow_state`: they mutate, and deliberately carry no `dry_run` — the
  rule is about verbs that write a **project file**, where a wrong write is
  expensive to undo. Both lists above are the live inventory; treat a verb on
  neither as unclassified rather than compliant.

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

3. **Resolve `caller_cwd` through the one helper.** If the tool is
   project-scoped, resolve the root via `ants::resolveCallerCwdRoot`
   (ANTS-1401, `src/resolvedroot.h`) — never re-implement
   canonicalisation or tab-walks inline. Read vs write routing is
   asymmetric (see the state-routing bullet above): `session_memory` /
   `workflow_state` writes into
   `~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json` go through the
   focused-tab gate; reads anchor to `caller_cwd` directly. The rule is
   about that store, not about per-project caches generally — a new
   cache under a `sha256(cwd)` path is not in scope merely for being
   keyed that way. (`RcGate` itself is used more widely, for
   caller_cwd checks unrelated to this store.)

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
   wrap — the canonical bypass list is the inline `isControlPlane`
   predicate at the `tools/call` dispatch site in
   `src/claudeintegration.cpp` (ANTS-1294 REG-4); there is no
   `isControlPlaneTool()` accessor, and `mcp_output_sanitisation`
   asserts that set exactly, so do not extract one. Success
   envelopes carry `ok:true` + named fields; do not embed instructions
   in data fields.

6. **Use the canonical refusal shape.** Every failure is
   `{ok:false, error:"<human readable>", code:"<taxonomy code>"}` with
   `code` drawn from [mcp-error-codes.md](mcp-error-codes.md). Reuse an
   existing code before minting a new one; if you mint one, add it to
   that doc in the same change.

   **6a. Writer/reader format parity (ANTS-2042).** When a
   discovery/reader verb recognises a target format that the paired
   *writer* can't yet produce, the writer MUST refuse with a
   format-aware code — carrying the discovered `format`, the `path`
   to the recognised file, and a format-appropriate Edit-fallback
   `hint` (naming the discovered format's append shape, not a bare
   "use Edit") — never a generic absence code (`no_*`, `*_not_found`). A generic absence code lies to the caller: their
   reader already saw the file, so "not found" sends them chasing a
   phantom-missing artifact instead of reaching for Edit. The rule
   applies whenever reader and writer discovery can diverge —
   `project_layout` discovering a `data/changelog.yaml` the
   Keep-a-Changelog writer can't append to, or `roadmap_query` parsing
   a `#### Pass N.M` heading roadmap that `roadmap_log`'s
   `create_section` can't splice — though `op:"append"` *does* render
   a pass block on that format (ANTS-2126 / ANTS-4117), which is why
   this is decided per op and not per verb.
   Instances: ANTS-2031 (roadmap_log returns `format_mismatch` instead
   of `bullet_not_found` on pass-headings — per **op**, not per verb;
   `amend_body` on that same file returns `unsupported_format`),
   ANTS-2040 (changelog_log
   returns `format_mismatch` instead of `no_changelog` on YAML
   changelogs). The generic absence codes (`bullet_not_found`,
   `no_changelog`) are **not** retired — they remain correct for the
   genuinely-absent case (no roadmap bullet / no changelog of any
   kind); `format_mismatch` is reserved for the *discovered-but-
   unwritable-format* branch. The `format_mismatch` code is defined in
   [mcp-error-codes.md](mcp-error-codes.md); reuse it rather than
   minting a per-verb variant. **Three codes are in play, and the choice
   is per *op*, never per verb:** `unrecognised_format` when the reader
   parsed nothing at all; otherwise choose between `unsupported_format`
   and `format_mismatch` by what **this op's** writer can produce.
   `roadmap_log` emits both on the *same* pass-headings file —
   `create_section` → `format_mismatch`, `amend_body` →
   `unsupported_format` — while `op:"append"` writes that format.

   **The boundary between those two is contested; ANTS-4134 is resolving
   it in [mcp-error-codes.md](mcp-error-codes.md), which owns the
   taxonomy.** That document's `format_mismatch` row currently glosses
   the code as "the whole verb cannot write the format", which the
   shipped behaviour above contradicts — five sibling ops write the
   format the `format_mismatch` case refuses. Until it lands, follow the
   per-op rule and the worked pair above, and do **not** derive a
   verb-level test from either document.

   **6b. Every *mutating* verb takes `dry_run` (ANTS-2077 / 2136 /
   2227).** The contract is stated in full in the quick-reference map
   above; it is repeated here as a checklist step because a mutating
   verb walked through steps 1–11 would otherwise ship without it.

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
   [ANTS-4108](../specs/ANTS-4108-spec-conformance-verb.md), and the
   `Inv9EtagShortCircuitIsHandlerLocal` case in
   `tests/features/spec_conformance_verb/`, which asserts the absence.
   Per-run measurement in the envelope is the only condition that earns
   this; wanting a different hash is not one. A handler-local 304 still
   returns the same `{ok:true, unchanged:true, etag}` shape, and a refusal
   envelope is never short-circuited.

   Two obligations come with it, and a verb missing either is broken in a
   way no test of the handler can see. **It still declares an `etag_match`
   input property in its schema — inline, not via `makeEtagMatchProp()`,
   with a description naming the fields its etag excludes.** Only the
   `isEtagSupportedTool` entry is withheld, never the property: an
   undeclared arg is dropped by the dispatcher and reported back in
   `ignored_args` (it is *not* rejected), so the handler never sees it and
   the 304 is silently unreachable. The shared factory is wrong here for a
   second reason: its description names no excluded fields, so a
   handler-local etag computed over a subset would ship undocumented.
   **It declines `fields=` (step 8)**: central projection is skipped only
   on a *central* 304, so a handler-local one is invisible to it and a
   caller passing `etag_match` and `fields` together would have
   `unchanged` projected out of its own 304 response.

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

The dispatch order is load-bearing: idempotent-read cache →
`applyEtagPattern` → `mcp::projectFields` → `<ants_mcp_data>` wrap. A
new opt-in (a future projection-like transform) slots into that chain;
read `applyEtagPattern`, `mcp::projectFields` and
`ClaudeIntegration::wrapMcpData` for the exact hook points before
adding one.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 / Q2 / Q3 / Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 3 (same doc, independent, cold) | 2 / 3 / 3 / n-a | **First gate on this document**, triggered by ANTS-4129's edit to step 7. 8 verified, 0 dismissed, all fixed. Q4 is not asked of a standard. All three lanes independently led on the same two defects, both in the new step-7 exception and both the same shape — it sanctioned a handler-local 304 without stating what else comes with it: the verb must still **declare** an `etag_match` schema property (step 10's `additionalProperties:false` rejects the arg otherwise, so the 304 is unreachable while every handler-level test still passes), and it must **decline** `fields=`, which the linked ANTS-4108 § 2.3 records as the second forced deviation. The `fields=` defect was also found by the orchestrator while building the packet. Verified against the dispatch predicate, not inferred: `etagUnchanged` is only ever set inside the `isEtagSupportedTool` branch, so a handler-local 304 always falls through to `mcp::projectFields` and a caller sending `etag_match` + `fields` loses `unchanged` from its own 304. Pre-existing defects the same read surfaced: the `fields=` quick-reference claimed the projection set is "a subset of the ETag set" — it is not, `read_log` projects but does not 304 (13-name set vs 25-name set, compared element-wise); § 6a's `format_mismatch` MUST was unqualified where `mcp-error-codes.md` reserves the narrower `unsupported_format` for the per-op gap, which `roadmap_log` emits live (`amend_body` → `unsupported_format`, `create_section` → `format_mismatch`, both predicates opened); `dry_run` was a hard obligation stated only in the at-a-glance map and absent from the checklist the document calls "the ordered procedure so a new tool doesn't miss a step" — added as **6b** rather than a new step, because ANTS-4108 and ANTS-2021 cite steps 7 and 8 by number and renumbering would strand them; step 10 permitted two of step 2's four contract words; and three pointers sent authors to `CLAUDE.md` § Conventions for wrap mechanics, hook points and a state-routing note that section has never contained. **Resolved clean, so not in the tally:** step 4's `validatePath` signature and defaults, and step 2's `Q_ASSERT_X` contract-drift assertion — both checked against source, both accurate; the lanes could not check them only because the packet lacked those windows. |
| 2 | 2026-08-12 | 3 (same doc, independent, cold, same packet rebuilt) | 2 / 2 / 2 / n-a | 6 verified, 0 dismissed, all fixed. **Half were loop 1's own collateral**, which is the honest headline. All three lanes led on the same one: loop 1's § 6a rewrite asserted `format_mismatch` is for "a format the verb cannot produce on any op" — false, and refuted by the example beside it, since `roadmap_log` *does* write pass-headings on `append` (ANTS-2126 / ANTS-4117) while `create_section` still refuses `format_mismatch`. The rule was deleted rather than restated: `mcp-error-codes.md` owns that boundary and this document now defers to it, because a discriminator derived here was wrong twice. Two lanes also found the step-7 exception said to *declare* `etag_match` without saying **how** — the shipped exemplar builds it inline, and a conformer reaching for `makeEtagMatchProp()` ships a description that names no excluded fields for an etag computed over a subset. **Loop 1's stated mechanism for that obligation was itself wrong and is corrected here**: `additionalProperties:false` does **not** reject an undeclared arg — measured by calling a verb with one, the dispatcher drops it and reports `ignored_args` (`src/claudeintegration.cpp:11750-11764`), so the 304 is unreachable silently rather than loudly. Loop 1's row is left as written; this row is the correction. Pre-existing defects found: the `Instances:` line stated ANTS-2031's `format_mismatch` for the verb when it is per-op; and the step-1 template broke the line between `registerToolProvider(` and `"<name>",` while the test it prescribes scrapes for them adjacent — 87 registrations in `mainwindow.cpp` use the adjacent form and none the split one, so a conformer copying the template ships a registration its mandated test cannot see. Also scoped "tenant-hashed storage", a phrase used once and never defined. **Three further errors were caught inside this loop's own fix pass by executing each new claim**: the factory description does not "promise an etag over the whole response" (it names no scope at all), `RcGate` is used far beyond the two state verbs, and a positive discriminator was asserted where none could be grounded — all three corrected before the commit. **Filed, not fixed here:** `mcp-error-codes.md`'s `format_mismatch` row defines the code as "the whole verb cannot write the format" while its own example has five sibling ops writing that format — a contradiction in the document that owns the taxonomy, out of scope for this one. Resolved clean, not in the tally: `PathValidation::validatePath` — the qualifier is right, `src/pathvalidation.h:13` opens `namespace PathValidation`. |
| 3 (cap) | 2026-08-12 | 3 (same doc, independent, cold, packet rebuilt) | 5 / 2 / 1 / n-a | 8 verified, 0 dismissed, all 8 fixed. **Stopped at the `--max-loops` cap, NOT converged** — see the note below the table. Sharpest find: `isControlPlaneTool()` was named as the "canonical bypass list" and **exists nowhere in the tree** — the real one is an inline `isControlPlane` predicate at the `tools/call` dispatch site, and `mcp_output_sanitisation` scrapes for that literal, so an author extracting the accessor the doc named would have reddened the suite. That defect was **inside the packet's own "settled, do not re-confirm" list**, which claimed every unresolved identifier had been spot-checked; the lane overrode the instruction and said so, which is exactly the behaviour the brief asks for when a finding contradicts a stated fact. Also: "`Q_ASSERT_X`, i.e. debug builds" understated the enforcement — ANTS-1834 makes `registerToolProvider` **refuse the registration in every build config**, so drift makes the tool go missing rather than run mis-classified. § 6a's *trigger example* still said the bullet writer "can't splice" pass-headings, contradicting the per-op rule three lines below it. `dry_run`'s "every mutating verb" was false against its own lists — `audit_dismiss` ships one and was unlisted, while `session_memory` / `workflow_state` mutate and deliberately carry none; the rule is now scoped to verbs that write a **project file**. The quick-reference ETag bullet stated the opt-in unqualified against step 7's exception — a judgement call left open at loop 1 on the grounds that a cold loop should decide it, and it did. **Two findings were loop 2's own collateral**: deferring § 6a's boundary wholly to `mcp-error-codes.md` pointed at a gloss the shipped code contradicts, and deleting the discriminator left the MUST with no selection test at all — unfalsifiable at authoring. § 6a now states the per-op rule the code implements and names the contested part explicitly (ANTS-4134) instead of asserting or deferring. **One error was caught inside this loop's fix pass by executing the claim**: the drift log is `ANTS_LOG`, not the `qWarning` two lanes named — they reasoned from a *paraphrase in my packet*, and reading the source gave a stronger and different answer (refusal, not a warning). |

**Not converged at the cap.** The remaining tail is one item: § 6a's
`format_mismatch` / `unsupported_format` boundary cannot be stated correctly
here until **ANTS-4134** fixes the definition in `mcp-error-codes.md`, which
owns the taxonomy and currently contradicts the shipped code. § 6a records
that openly rather than guessing, so an implementer knows the boundary is
contested. Loop counts here are **not** a size signal — at 372 lines this
document is mid-pack among its siblings. The cap bound because this is a
**hub**: it restates contracts owned by `mcp-error-codes.md`, `mcp-caches.md`,
`mcp-behavioural-notes.md` and the specs, and every restatement is an
independent chance to drift from its owner. Three loops each found drift of
exactly that kind. The durable fix is fewer restatements and more pointers,
which is a restructuring job, not a review finding.
