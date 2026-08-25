# ANTS-1720 — MCP response projection (`fields=` parameter)

## Problem

High-volume MCP read tools return their full payload on every call even
when the caller needs one top-level field. `roadmap_query` alone can run
8–80 KB. A caller polling for `git_state`'s `branch`, or scraping
`roadmap_query`'s `bullets`, pays for the entire envelope each round-trip.

## Solution

A new optional `fields: ["f1","f2"]` array parameter on eleven read tools
returns **only the named top-level fields**. The response *schema* is
unchanged — absent fields are simply omitted. Callers that omit `fields`
get the full payload (fully backwards-compatible).

Tools in scope (the original seven are etag-supported, ANTS-1499; of the
four later additions all are etag-supported except `read_log`, which is
projection-only):

`roadmap_query`, `project_layout`, `file_outline`, `get_environment`,
`tab_list`, `subsystem`, `git_state`, `read_log` (ANTS-1855),
`model_switch_stats` (ANTS-1735), `read_region` (ANTS-2021),
`codebase_index` (ANTS-1637).

## Where it lives

Pure projection logic is `mcp::projectFields` + the allowlist
`mcp::isFieldProjectionTool` in `src/mcpprojection.{h,cpp}` (Qt6::Core
only — so the dispatch layer and this test share one implementation,
mirroring `focusedtest` / `modelrecommender`).

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
- **INV-8 — allowlist is the in-scope tools only.**
  `isFieldProjectionTool` returns true for exactly those and false
  otherwise (e.g. `get_scrollback`, `session_brief`). The count is
  deliberately not written here: it read "eleven" while the list held
  fourteen, because every addition (ANTS-3533, ANTS-2139, ANTS-3368,
  ANTS-4523 …) updates the list and not the prose. The test asserts the
  membership, which is the part that matters; a number no check reads is
  a number that goes stale.

  **`session_orient` joined the list under ANTS-4523**, having been absent
  by omission — the negative list below names `session_brief` and
  `current_state` and never named it. **That fix was incomplete and the gap
  shipped (ANTS-4624)**: membership makes the dispatcher willing to project,
  and the schema property is what lets the argument arrive projectable. See
  INV-10.

  **The positive list was widened to the full set by ANTS-4624.** It named
  thirteen of fifteen — `changelog_query` (ANTS-3533) and `co_change_family`
  (ANTS-3368) were on the allowlist and asserted by nothing, in a test
  calling itself Exact.

  **Known contradiction, filed not fixed.** `tests/features/mcp_ignored_args`
  INV-2 calls `fields` a universal dispatch-layer arg that is NEVER reported
  as ignored, "because the dispatcher accepts them for every verb". This
  allowlist is why that premise is false: a verb outside it accepts `fields`,
  drops it, and is barred from reporting it — silence in both directions.
  **The shared predicate was split by ANTS-4524 route 1 (2026-08-25)**, which
  was the blocker named here: `compact` no longer shares this gate.
  `isFieldProjectionTool` answers `fields=`; `isDefaultCompactTool` answers
  whether an ABSENT `compact` falls back to the default-ON terse setting, and
  both are columns of one table so a new verb must answer each. Widening this
  list therefore no longer compacts a verb nobody asked to compact — the reason
  the fix was not free. The contradiction with `mcp_ignored_args` INV-2 stands;
  making `fields` genuinely universal is the remaining half.
- **INV-9 — dispatch ordering.** In `claudeintegration.cpp` the
  `projectFields` call appears after `applyEtagPattern` and before the
  `wrapMcpData` call, and is guarded so the etag short-circuit
  (`{ok,unchanged,etag}`) is never narrowed.
- **INV-10 — schema declares `fields`.** Every tool on INV-8's list carries
  a `fields` array-of-string property in its `inputSchema.properties`.

  **ANTS-4624 — the count is now DERIVED, and that is the invariant.** This
  read "each of the eleven tools" while the list held fourteen, and the test
  asserted a hardcoded fourteen `makeFieldsProp()` call sites. So when
  ANTS-4523 added `session_orient` to the allowlist the count still matched
  the fourteen verbs that DO declare the property, INV-10 kept passing, and
  the one member without it shipped. INV-8's own note — *a number no check
  reads is a number that goes stale* — was true of INV-10 and nobody applied
  it there.

  The cost was silent: the dispatcher requires `fv.isArray()`, an undeclared
  property gives the client no type to marshal to, so `fields` arrived as a
  string and the projection was skipped on the largest response in the
  session. Nothing reported it — `ignored_args` is barred from naming
  `fields`, and passing the argument suppresses the very hint that
  recommended it.

  Both invariants now read one `kFieldProjectionTools` array in the test, and
  INV-10 asserts `std::size` of it. Adding a tool to the allowlist without a
  schema property fails INV-10; adding a schema property without the
  allowlist fails INV-8. Do not reintroduce a literal.
- **INV-11 — `fields=` and the compaction DEFAULT are separate answers**
  (ANTS-4524). `mcp::isFieldProjectionTool` grants `fields=`;
  `mcp::isDefaultCompactTool` decides whether an ABSENT `compact` falls back to
  `mcp::terseDefault()`. Both read one table in `mcpprojection.cpp`, so adding
  a verb makes you answer each. `spec_lint` is the row where they differ, and
  the test asserts that difference — if no row differs, the split has collapsed
  back into a rename. An EXPLICIT `compact` is still honoured wherever the
  schema declares it; only the unasked-for default is gated, because
  withdrawing a declared argument would silently drop it, which is the same
  defect pointing the other way.

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
