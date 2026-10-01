# mcp_indie_review_tools — indie_review_* MCP tools wired to RemoteControl

Locks the wiring + envelope shape of the `indie_review_*` MCP tools
(ANTS-1112). ANTS-5485 removed `indie_review_brief`,
`indie_review_synthesis_prompt`, `indie_review_fold_in` and
`indie_review_orchestrate`; `indie_review_partition` and
`indie_review_corroborate` remain.

## INVs

- INV-9 (per-name registration): each remaining tool name appears in
  `tools/list` (source-grep against `claudeintegration.cpp`).
- Each remaining name is registered with `registerToolProvider`
  (source-grep).
- Each remaining cmdIndieReview* method exists in `remotecontrol.h`
  (signature source-grep) and the RemoteControl TUs (definition
  source-grep).
- INV-10: an envelope round-trip — feed JSON
  `{lane1: report1, lane2: report2}` where both report `src/foo.cpp:42`
  and verify the response contains a finding with both lanes — is
  exercised by the existing `IndieReviewEngine::corroboratedFindings`
  test (no MCP roundtrip needed; the MCP handler is a thin wrapper
  delegating to the engine, so testing the engine + the wiring
  separately is sufficient).

## ANTS-1581 reversal — the "Parallel API" note inverts

ANTS-1581(b) appended "the `/cold-eyes` / `/indie-review` skill
orchestrates this step itself and does not call this tool" to every
`cold_eyes_*` / `indie_review_*` description, so naming parity would not
read as "this is the canonical path"; ANTS-3639 then exempted the verbs
the skills *do* mandate. Global `CLAUDE.md` §18 (2026-07-28) reverses the
premise: the MCP verbs are the default path and the raw tools are the
fallback, so the note now argues against the standing rule.

One carve-out survives, and it is about what the verb **does**, not who
calls it — `indie_review_dispatch` runs each lane on the project's
configured local endpoint (default `llama3`), a different and weaker
reviewer rather than a cheaper route to the same review.

- INV-14 (reversal is total, not narrowed again): the string
  `Parallel API:` does not occur in `claudeintegration.cpp`. The one
  surviving note is gated on `name == QLatin1String("indie_review_dispatch")`
  — a name equality, not a family prefix match — and that guard precedes
  the note text it appends.
- INV-15 (catalog hint agrees): within `indie_review_dispatch`'s own
  descriptor block, `selection_hint` names the `LOCAL AI endpoint` the
  review runs on and no longer calls the verb the `entry-point
  orchestrator` for a review. The hint is what `tool_info {catalog:true}`
  shows a session choosing a verb, so it is the same defect surface as
  the description.
