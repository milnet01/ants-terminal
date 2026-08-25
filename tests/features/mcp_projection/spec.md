# ANTS-1720 — MCP response projection (`fields=` parameter)

## Problem

High-volume MCP read tools return their full payload on every call even
when the caller needs one top-level field. `roadmap_query` alone can run
8–80 KB. A caller polling for `git_state`'s `branch`, or scraping
`roadmap_query`'s `bullets`, pays for the entire envelope each round-trip.

## Solution

An optional `fields: ["f1","f2"]` array parameter returns **only the named
top-level fields**. The response *schema* is unchanged — absent fields are
simply omitted. Callers that omit `fields` get the full payload (fully
backwards-compatible).

**Tools in scope: every verb (ANTS-4524).** It began as an allowlist of
high-volume reads and grew one verb at a time (ANTS-1855, ANTS-2021,
ANTS-1637, ANTS-1735, ANTS-3533, ANTS-2139, ANTS-3368, ANTS-4523, ANTS-4429,
ANTS-4663, ANTS-4665). Each of those was a caller who had passed `fields=`,
had it dropped in silence, and reported it — so the allowlist was a queue of
future defects rather than a safety property. The transform never asked which
verb it was serving.

## Where it lives

Pure projection logic is `mcp::projectFields` in `src/mcpprojection.{h,cpp}`
(Qt6::Core only — so the dispatch layer and this test share one
implementation, mirroring `focusedtest` / `modelrecommender`). There is no
tool-name predicate; the dispatch site calls it for every verb.

The dispatch site (`ClaudeIntegration`, `tools/call` branch) calls
`mcp::projectFields` **after** the ETag short-circuit and **before**
`wrapMcpData`. This ordering is the load-bearing invariant: the etag is
computed on the *unfiltered* canonical body, so a narrowed call still
short-circuits when state is unchanged.

## Invariants

- **INV-1 — full payload on absent/empty `fields`.** `projectFields(body,
  emptyArray)` returns `body` byte-for-byte. The dispatch only calls
  `projectFields` when `fields` is a present, non-empty array.
- **INV-2 — single-field subset correct.** `projectFields` of a body with
  `fields=["bullets"]` returns an object containing exactly the `bullets`
  key, with its value copied verbatim.
- **INV-3 — multi-field subset preserves only named keys, verbatim.**
  `fields=["branch","files"]` yields `{branch,files}` and nothing else.
- **INV-4 — unknown field name is never an error, and is REPORTED.**
  (ANTS-4567) A name the envelope does not carry is listed in
  `fields_unmatched`, emitted only when non-empty — so an exactly-matching
  request is byte-identical to before, while an all-unknown list yields that
  field alone instead of the bare `{}` it used to. The bare `{}` was the
  defect: it is also what a CORRECT request for entirely inapplicable fields
  returns, so a caller probing a verb could not tell a true "none of these
  apply" from a misspelling, from compaction having stripped the survivors, or
  from the argument being dropped altogether. Absence was carrying the answer.
  This is ANTS-4578's pattern applied to the VALUES of `fields` rather than to
  the argument.
  `fields=["nonexistent"]` returns `{}`. A mix of known + unknown returns
  only the known keys.
- **INV-5 — non-string / empty field entries are ignored**, not faulted.
- **INV-6 — non-object response bodies pass through unchanged** (the
  projection only applies to JSON-object envelopes).
- **INV-7 — etag is NOT auto-preserved.** To keep the etag for a
  follow-up 304 call, the caller lists `"etag"` in `fields`. Because the
  dispatch computes the etag on the unfiltered body *before* projecting,
  a `fields=["bullets","etag"]` response carries the same etag a full
  call would (this is what "etag computed on canonical body, not filtered
  body" means).
- **INV-8 — there is no allowlist (ANTS-4524).** `fields=` is honoured on
  every verb, and the predicate that gated it is deleted rather than left
  dormant — a dormant one is what the next verb-author reaches for. *Test:*
  `Inv8NoFieldsAllowlist`, which narrows an envelope the old list excluded
  and scrapes `mcpprojection.cpp` for the absence of the symbol.

  **A 304 is never narrowed.** The dispatcher already skips projection on the
  central etag short-circuit, but a verb carrying per-run measurements owns
  its own 304 (`mcp-tools.md` step 7, `spec_conformance`) and that one is
  invisible to the dispatcher. While the allowlist existed such a verb was
  told to *decline* `fields=`; there is nothing to decline now, so the floor
  moved into `projectFields`, where it covers both. *Test:*
  `Ants4524NeverNarrowsA304`.

  **What the allowlist cost while it stood**, kept because it is the argument
  against reintroducing one: `session_orient` joined it under ANTS-4523 having
  been absent by omission, and that fix was incomplete — membership makes the
  dispatcher willing to project, the schema property is what lets the argument
  arrive projectable, and the gap shipped (ANTS-4624, see INV-10). The
  positive test list then named thirteen of fifteen, in a test calling itself
  Exact.

  **The contradiction with `mcp_ignored_args` INV-2 is closed, and it took two
  changes because the second was unsafe until the first landed.** INV-2 called
  `fields` a universal dispatch-layer arg never reported as ignored, while the
  allowlist meant a verb outside it accepted `fields`, dropped it, and was
  barred from saying so — silence in both directions. ANTS-4578 made the
  advisory report an unhonoured `fields`, which named the problem. ANTS-4524
  removed it: first splitting `compact` off this predicate (widening a shared
  gate would have compacted every unlisted verb for callers who never asked),
  then deleting the gate. Both specs were edited together, as each was true
  alone and false as a pair — which is how the contradiction survived.
- **INV-9 — dispatch ordering.** In `claudeintegration.cpp` the
  `projectFields` call appears after `applyEtagPattern` and before the
  `wrapMcpData` call, and is guarded so the etag short-circuit
  (`{ok,unchanged,etag}`) is never narrowed.
- **INV-10 — the schema declares `fields` on EVERY verb.** The `tools/list`
  builder injects the property into any verb whose schema does not already
  carry it, in the same per-tool loop as the ANTS-1520 `caller_cwd` injection
  and before the `m_lastToolsList` snapshot that `tool_info` and the
  `ignored_args` cache read. A verb declaring its own richer `fields`
  description keeps it; the injected one is terse, because ~150 copies of the
  full description is real wire cost on `tools/list`. *Test:*
  `Inv10SchemaDeclaresFieldsUniversally`.

  **Why the invariant exists at all — ANTS-4624.** Dispatcher willingness and
  the schema property are two halves, and half is silent: `session_orient`
  joined the allowlist without the property, so the client had no type to
  marshal to, `fields` arrived as a string, `fv.isArray()` was false, and the
  narrowing was skipped on the largest response in the session. Nothing
  reported it — `ignored_args` was barred from naming `fields`, and passing
  the argument suppressed the very hint that recommended it. Under ANTS-4524
  the same gap is available for every verb at once, which is why the
  declaration is injected rather than hand-written.
- **INV-11 — declaring `compact` and defaulting to it are separate answers**
  (ANTS-4524). `mcp::isCompactArgTool` says the verb declares `compact`, so an
  EXPLICIT `compact:true`/`false` is honoured; `mcp::isDefaultCompactTool`
  says whether an ABSENT one falls back to `mcp::terseDefault()`. Both read
  one table in `mcpprojection.cpp`, so adding a verb makes you answer each.
  `spec_lint` and `feedback_query` are the rows where they differ, and the
  test asserts at least one does — if none does, membership has silently
  become the default again.

  **Compaction did not follow `fields=` into being universal, and the reason
  is the default.** `fields=` is opt-in with no default, so widening it can
  only do what a caller asked for. Compaction has one, and
  `claude.mcp_terse_responses` sets it ON at startup — so a verb joining the
  shared predicate for narrowing began compacting for every caller. That is
  how ANTS-4663 shipped ANTS-4673, folding away the `sections_checked:false`
  that says a check never ran.

  Why it exists: the two shared one predicate, so ANTS-4663 added `spec_lint`
  for `fields=` and compaction arrived with it, folding away the
  `sections_checked:false` that says a check never ran (ANTS-4673). The
  coupling was named in ANTS-4524 before it was hit.

## ANTS-2090 — tabular (columnar) encoding (`encoding:"tabular"`)

Same bundle, separate transform: `mcp::tabularize` packs each eligible
top-level array-of-objects into a columnar `{__cols__,__rows__}` form (one
sorted header row + one value-row per element), dropping the per-row key
repetition that dominates list replies. Opt-in per call
(`encoding:"tabular"`), self-guarding per array. Full design + cold-eyes
log: `docs/specs/ANTS-2090.md`. Test invariants (`McpTabular` cases):

- **INV-1/8/9 — dispatch order + guard.** `mcp::tabularize` is called
  only under an `encoding == "tabular"` guard, after `mcp::appendReadHints`
  and before `mcp::offloadBody` (source-scrape, by symbol not line number).
- **INV-2 — eligibility.** Transformed iff a top-level array is non-empty,
  has ≥2 elements, and every element is a JSON object; empty / single /
  scalar / non-object-bearing arrays pass through unchanged.
- **INV-3 — round-trip, length-preserving.** Zipping `__cols__` with each
  `__rows__` entry reconstructs the original array (order preserved,
  one row per element so a sibling `count` stays consistent), faithful up
  to the missing-key ⟺ explicit-null collapse.
- **INV-4 — never costs bytes.** Per array, the columnar form is emitted
  only when strictly smaller (compact UTF-8); otherwise the array is kept.
- **INV-5 — nested values verbatim.** Object/array cell values (`lanes`,
  `also_at`, …) are carried into the row unchanged; no recursion in v1.
- **INV-6 — refusal/non-object floor.** A non-object body or an `ok:false`
  envelope is returned unchanged.
- **INV-7 — determinism + lexicographic `__cols__`.** Identical input →
  byte-identical output; the column union is sorted regardless of element
  order.
- **Schema** — the 7 list-shaped read verbs (`roadmap_query`,
  `workspace_search`, `file_outline`, `find_sources`, `find_caller`,
  `codebase_index`, `docs_index`) each declare the `encoding` enum prop.

## ANTS-3550 — advisory-hint "already-taught" latch

Same bundle, `appendReadHints` chokepoint: the presentation-only nudges
(`next_call_hint` / `leaner_call_hint`) are educational the first time and
pure recurring overhead thereafter. A process-scoped latch emits each
`(tool, hint-kind)` pair once, then suppresses it. State lives in
`src/mcpprojection.cpp` (`setHintLatchEnabled` / `hintLatchEnabled` /
`resetHintLatch` + the private `claimHint` test-and-set). Module default
OFF (so the `McpReadHints` cases above — and any direct library use — emit
a hint on every call, exactly as pre-3550); the app turns it ON via the
`claude.mcp_hint_latch` config key (default true), published to the module
flag from `mainwindow.cpp` at config load + external reload (mirrors
`setTerseDefault`). Getter-only config — the off-switch is the config key,
live-reloaded on external edit; no Settings toggle. Test invariants
(`McpHintLatch` / `McpHintLatchWiring` cases):

- **INV-L1 — module default OFF; getter/setter round-trip.**
  `hintLatchEnabled()` is false at rest; `setHintLatchEnabled` flips it.
- **INV-L2 — enabled: first emission passes, exact repeat suppressed.** With
  the latch on, the second identical `appendReadHints(tool,…)` omits the
  nudge it emitted on the first (a body with both nudges suppressed returns
  unchanged).
- **INV-L3 — keyed per `(tool, hint-kind)`.** A distinct verb's own nudge is
  never hidden by another verb being taught — `workspace_search`'s leaner
  tip and `file_outline`'s etag nudge still emit after `roadmap_query` was
  taught; only exact `(tool, kind)` repeats drop.
- **INV-L4 — the two kinds latch independently.** Teaching a tool's
  `next_call_hint` does not consume its `leaner_call_hint` slot.
- **INV-L5 — disabled ⟹ no suppression.** With the latch off, repeated calls
  emit every time (byte-for-byte the pre-3550 behaviour).
- **INV-L6 — `resetHintLatch()` re-teaches.** After a reset the next call
  emits the nudge again (test isolation / future per-session hook).
- **INV-L7 — wiring.** `config.cpp` exposes
  `claude.mcp_hint_latch` defaulting true; `mainwindow.cpp` publishes it via
  `mcp::setHintLatchEnabled(m_config.claudeMcpHintLatch())`.
