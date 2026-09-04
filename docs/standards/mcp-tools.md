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
- **Which thread the verb runs on (ANTS-2132)** — step 2a.
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
   // A plain cmd*() forward — the common shape, and off-thread (step 2a).
   m_claudeIntegration->registerToolProvider("<name>",
       ClaudeIntegration::CallerCwdContract::Required,   // step 2
       rcDelegate(&RemoteControl::cmdFoo));

   // A hand-written body — runs on the GUI thread (step 2a).
   m_claudeIntegration->registerToolProvider("<name>",
       ClaudeIntegration::CallerCwdContract::Required,   // step 2
       [this](const QJsonObject &args) -> QString { /* … */ });
   ```

   **Both forms are shown because the registration form picks your thread —
   unless the contract is `TabSpecific`, which forces the GUI thread
   whichever form you use.** Step 2a reads BOTH facts back off this call,
   not just the third argument. The lambda used to be
   the only form shown here while step 2a called the factory form "the
   common case" — which it is: the factory is the majority of registrations
   in `mainwindow.cpp`, so the documented shape was the minority one.

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
   `ProcessGlobal` buy no dispatch-time check (ANTS-1404 Phase 3a, and the
   enum's own comments say so), so a tab-scoped verb gates itself in its
   handler — `RcGate` is the shared helper for that, and step 3 notes it is
   used well beyond the state store. An unclassified tool defaults to
   `Optional`. Picking the word does not buy the check: the drift assert
   above compares your two declarations of it, and only `Required` becomes
   a call-time refusal.

   **`TabSpecific` is not a free label, though.** It is classification-only
   for *enforcement*, and step 2a reads it as a THREAD selector: a
   `TabSpecific` verb runs on the GUI thread whichever registration form it
   used. So the word you pick here decides what your handler may touch.

2a. **Know which thread your verb runs on (ANTS-2132).** Registration
   decides it, from two facts step 1 already carries. A verb built by the
   `rcDelegate` factory whose contract is **not** `TabSpecific` runs on the
   shared MCP dispatch worker; everything else — every hand-written inline
   lambda, and every `TabSpecific` verb — runs on the GUI thread. So a plain
   `cmd*()` forward is off-thread by default, and that is the common case.

   **An off-thread body must not touch a widget or a `MainWindow` accessor
   directly.** Marshal the read through `ants::onGuiThread`
   (`src/guithread.h`), which returns `std::nullopt` when the dispatcher is
   shutting down and refused it. On `nullopt` refuse with your own
   anchor-failure code — never fall back to a default-constructed value, which
   answers with a silently wrong project instead of an error the caller sees.

   Two consequences worth knowing. Verbs still execute one at a time, in
   arrival order, so nothing that could not overlap before begins to. And an
   over-cap call is refused with `dispatch_queue_full` before the handler runs
   — nothing for the handler to do, but a caller may see it.

   `tests/features/mcp_verb_offthread_guard/` locks the structure and
   `tests/features/mcp_async_dispatch/` the runtime behaviour.

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

   **A refusal envelope is never short-circuited, and never carries an
   etag — on EITHER arm.** A 304 answers "your cached copy is still
   current", and a refusal is not a copy of anything: replying `unchanged`
   to one tells the caller a call that just failed had succeeded. So the
   dispatcher parses the body and tests `ok` BEFORE the 304 arm, rather
   than speaking for a body it has not read (ANTS-4446). Withholding the
   etag is the half that matters, because a caller can only replay a value
   it was given. The handler-local exception below inherits this rule
   unchanged — it is stated here, above the split, because it binds both.

   Not the same rule, though it is adjacent: the idempotent-read cache's
   INV-5(a)/(b) refuse to *store* an empty or transient-unavailable
   response. That is about what may enter a cache; this is about what may
   be served from one. Each guards its own side rather than trusting the
   other.

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
   returns the same `{ok:true, unchanged:true, etag}` shape, and the
   refusal rule stated above binds it too.

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
   it by existing. It narrows to named top-level fields and has three
   floors. A **304** (`unchanged:true`) is returned whole — so compose with
   ETag by listing `"etag"` in `fields`. A **refusal** is narrowed like any
   other envelope, then `ok` / `code` / `error` / `retry_after_ms` are
   re-inserted — so § 6a's `format`, `path` and `hint` survive only if the
   caller named them. And a **diagnostic** — `warning` and
   `parseable_bullets` — is re-inserted into every narrowed envelope,
   success as well as refusal (ANTS-4698): a reply must not lose the field
   that says how to read the fields it kept. Step 5a's *prefer a count* is
   about COMPACTION and does not compete with this: a count survives
   folding, a `warning` survives folding and narrowing both.

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
   So *if you take a row at all*, a field a caller branches on needs BOTH,
   not either: a `defaultCompact:false` row, which spares the caller who did
   not ask, and a surviving shape — the `_checked` suffix, or a count — for
   the caller who passes `compact:true` (step 5a).

   **Where a meaning-bearing `false` is your only reason to consider a row,
   take none.** The no-row verb above cannot be folded by anyone, which is
   the stronger protection — and adding a row is the one act that exposes
   the field to `compact:true`. Read the two paragraphs in that order, or
   the requirement reads as a reason to add the row that causes the harm.

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
over-threshold response is re-applied after it. The pre-offload apply is
skipped on a cache hit, because the cached body already carries the
advisory; the re-apply runs only when the body actually spills. So on a
cache hit under the threshold neither fires, and that is correct rather
than a gap. And `mcp::compactEnvelope` may fold your `null` / `false` / `""` /
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

Moved to [`docs/reviews/mcp-tools-review-log.md`](../reviews/mcp-tools-review-log.md).
A standard carries rules; its review history is read far less often and
was the larger half of this file.
