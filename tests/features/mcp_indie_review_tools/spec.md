# mcp_indie_review_tools — 5 new MCP tools wired to RemoteControl

Locks the wiring + envelope shape of the 5 `indie_review_*` MCP
tools (ANTS-1112).

## INVs

- INV-9 (per-name registration): each of the 5 tool names appears
  in `tools/list` with `inputSchema.type == "object"`. Source-grep
  against `claudeintegration.cpp` confirms 5 distinct entries.
- All 5 names appear in `mainwindow.cpp` `setupClaudeMcpProviders`
  as `registerToolProvider` calls (source-grep).
- All 5 cmdIndieReview* methods exist in `remotecontrol.h` (signature
  source-grep) and `remotecontrol.cpp` (definition source-grep).
- INV-10: an envelope round-trip — feed JSON
  `{lane1: report1, lane2: report2}` where both report `src/foo.cpp:42`
  and verify the response contains a finding with both lanes — is
  exercised by the existing `IndieReviewEngine::corroboratedFindings`
  test (no MCP roundtrip needed; the MCP handler is a thin wrapper
  delegating to the engine, so testing the engine + the wiring
  separately is sufficient).

## ANTS-3375 / ANTS-3493 — `source_paths[]` ad-hoc lane

The code-review analogue of `cold_eyes_brief`'s `doc_paths[]`
lane-agnostic fallback (ANTS-1508). Lets a session get a single
cold-review brief over an arbitrary changed-file set (a Rule-8
code/dependency diff) without committing a `.indie-review/partition.json`
— the "lightweight single-reviewer broker for a code diff" the
cross-session feedback asked for.

- INV-11 (handler ad-hoc fallback): `cmdIndieReviewBrief`, on a lane
  name that is **not** in the derived partition, reads a `source_paths`
  array from the request, anchors each entry through
  `PathValidation::validatePath` (INV-13 traversal guard, same
  chokepoint `cold_eyes_brief` uses), and synthesises an ad-hoc
  `IndieReviewEngine::Lane` from the accepted paths, then feeds it to
  `assembleBriefManifest`. Source-grep gate over the handler body.
- INV-12 (recoverable refusal): when the lane is unknown **and** no
  usable `source_paths[]` is supplied, the `not_found` refusal names
  the `source_paths[]` override and carries `known_lanes` (recover
  without a second `indie_review_partition` round-trip) plus
  `source_paths_rejected` (per-path reject reasons for entries that
  failed the traversal / existence check).
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

- INV-13 (schema + discoverability): the `indie_review_brief` tool
  descriptor in `claudeintegration.cpp` declares the optional
  `source_paths` array prop and its `description` cites ANTS-3375 /
  ANTS-3493 so the mode is discoverable from `tools/list`. Schema keeps
  `additionalProperties:false` (`source_paths` is now a known key).
