# mcp_ignored_args — unknown-arg advisory (ANTS-2175)

## Problem

The MCP `tools/call` dispatcher silently drops args a verb does not
declare. A `query="token"` passed to `roadmap_query` (whose filters are
`status` / `section` / `id` / `mode` — there is no `query`) was ignored,
and the verb returned the full unfiltered roadmap — which *looked* like a
working search. The typo cost several follow-up calls to notice the filter
was a no-op.

## Surface

- `src/mcpprojection.{h,cpp}` — `mcp::ignoredArgs(args, known)`: the pure
  diff (Qt6::Core only, in `ants_core_lib`, shared by the dispatch layer and
  this test, mirroring `projectFields` / `offloadRequested`).
- `src/claudeintegration.{h,cpp}` — `m_toolParamKeys` cache (rebuilt at the
  end of the `tools/list` handler from each tool's `inputSchema.properties`)
  and the dispatch-site injection that attaches `ignored_args` to the
  success envelope.

## Invariants

- **INV-1** — `ignoredArgs` returns, sorted ascending, exactly the keys in
  `args` that are neither in `known`, nor unconditionally universal, nor in
  the caller-supplied `honoured` set.
- **INV-2** — a dispatch-layer arg the verb HONOURS is never reported, even
  when its `known` set does not redeclare it. Three are unconditional:
  `caller_cwd`, which every verb reads; `encoding`, which the dispatcher
  applies with no tool-name predicate; and `fields`, universal since
  ANTS-4524. The rest arrive per call in `honoured`.
- **INV-2b** (ANTS-4578) — **`etag_match`, `compact` and `offload` ARE
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
- **INV-6** — the dispatch site calls `mcp::ignoredArgs`, attaches the field
  through `mcp::withIgnoredArgs`, and is gated so it runs only on a
  freshly-dispatched call (`!cachedHit`).
- **INV-7** — (ANTS-4525) the advisory is attached to **refusals** as well as
  successes, and only ever ADDS a key, so the ANTS-2112 refusal floor
  (`ok`/`code`/`error`/`retry_after_ms`) survives. An empty `ignored` list or
  an unparsable body returns the response byte-identical.

  INV-6 read *"never annotates a refusal (`ok:false`)"* until 2026-08-19, and
  that is backwards for the case it matters most in: a caller holding a wrong
  mental model of a verb passes wrong ARGUMENTS and gets a refusal, so the
  reply that would correct the model is suppressed precisely because it
  refused. Measured — a DOOM session called `read_log {max_commits:3,
  body:true}` believing it read git log; both args are unknown to the verb,
  nothing said so, and the reply was `not_found` on the Ants debug-log path,
  which explains the file rather than the misconception. Suppression stays
  defensible for a refusal *caused by* the unknown args; distinguishing that
  case costs more than the harmless duplication.

  **What is falsifiable against the pre-fix tree** is INV-6's scrape for the
  absent `!= QJsonValue(false)` guard. `mcp::withIgnoredArgs` is new, so its
  own tests could only fail to compile there.

## Tests

`test_mcp_ignored_args.cpp` — behavioural coverage of `mcp::ignoredArgs`
(INV-1..INV-4) plus source-scrapes of `claudeintegration.cpp` for the cache
population and dispatch injection (INV-5, INV-6).
