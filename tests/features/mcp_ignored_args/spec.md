# mcp_ignored_args — which arguments a verb does not declare (ANTS-2175)

## Problem

A `query="token"` passed to `roadmap_query`, which had no `query`, was
dropped in silence, and the verb returned the full unfiltered roadmap.
ANTS-2175 named such arguments in an `ignored_args` advisory. ANTS-4659
refuses the call instead; this contract keeps the diff both were built on.

## Surface

- `src/mcpprojection.{h,cpp}` — `mcp::ignoredArgs(args, known)`: the pure
  diff (Qt6::Core only, in `ants_core_lib`, shared by the dispatch layer and
  this test, mirroring `projectFields` / `offloadRequested`).
- `src/claudeintegration.{h,cpp}` — `m_toolParamKeys` cache (rebuilt at the
  end of the `tools/list` handler from each tool's `inputSchema.properties`)
  and the dispatch-site check that refuses a call naming one.

## Invariants

- **INV-1** — `ignoredArgs` returns, sorted ascending, exactly the keys in
  `args` that are neither in `known`, nor unconditionally universal, nor in
  the caller-supplied `honoured` set.
- **INV-2** — a dispatch-layer arg the verb HONOURS is never reported, even
  when its `known` set does not redeclare it. Three are unconditional:
  `caller_cwd`, which every verb reads; `encoding`, which the dispatcher
  applies with no tool-name predicate; `fields`, universal since
  ANTS-4524; and `compact`, universal since ANTS-4659. The rest arrive per
  call in `honoured`.
- **INV-2b** (ANTS-4578) — **`etag_match` and `offload` ARE
  reported on a verb that does not honour them.** They were exempt
  everywhere until 2026-08-25, while the dispatcher acts on them only for
  the verbs on its allowlists — so an arg sent to a verb with no support for
  it did nothing and reported nothing, and the caller believed it had worked.
  Reported from two projects, both about `fields`: on `feedback_query`, and
  on `session_orient` before it joined the projection allowlist. `raw` was
  never on the exemption list and was correctly reported throughout; that
  asymmetry is what identified the defect.

  **`fields` left this list by being fixed, not by being exempted again
  (ANTS-4524).** It is honoured on every verb now, so no verb can drop it —
  which is what INV-2 claimed all along and what made the original defect
  invisible. The distinction between the two exemptions is the whole lesson
  here and it is not visible from inside `ignoredArgs`: *exempt because it
  always works* and *exempt because nobody checked* produce the identical
  empty list. `Ants4524FieldsIsUniversalAndNeverFlagged` pins the first;
  `mcp_projection` INV-8 and INV-10 are what make it true.
- **INV-3** — when every arg is recognised (in `known` or universal), the
  result is empty.
- **INV-4** — an empty `known` set reports every non-universal arg (a verb
  that declares no properties still accepts the universal args).
- **INV-5** — the `tools/list` handler populates `m_toolParamKeys` from the
  assembled tools array's `inputSchema.properties`, rebuilt in lockstep with
  `m_lastToolsList`.
- **INV-6** — the dispatch site calls `mcp::ignoredArgs` for a verb whose
  schema it knows. What it does with the result is
  `tests/features/mcp_undeclared_args_refused/spec.md`'s.

## Tests

`test_mcp_ignored_args.cpp` — behavioural coverage of `mcp::ignoredArgs`
(INV-1..INV-4) plus source-scrapes of `claudeintegration.cpp` for the cache
population and the dispatch-site check (INV-5, INV-6).
