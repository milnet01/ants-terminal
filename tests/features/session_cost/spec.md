# Feature: session_cost — a session's context rent (ANTS-5698)

Every request a Claude Code session sends re-reads its whole context, so
most of a session's token cost is rent on what it already holds. This verb
reads one session's transcript and reports that rent: how large the context
was at the first request and is now, the session's token totals, the
start-up payload by part, and the largest items the session holds.

It reads files only. It runs in `ants-mcpd`, so a change reaches a session
on its next `/mcp` reconnect; no terminal relaunch.

## Inputs

- `caller_cwd` (required) picks the transcript folder: the harness names it
  after the session's cwd with every character outside ASCII letters and
  digits replaced by `-`. A cwd with no folder is retried at each ancestor.
- `session_id` (optional) names the transcript. Absent, the newest
  transcript in the folder is read, the reply says `session_inferred: true`,
  and `other_active_sessions` counts the folder's other transcripts written
  in the last ten minutes. `"agent-<id>"` reads a helper agent's transcript,
  `<folder>/<session>/subagents/agent-<id>.jsonl`. `"self"` reads the
  newest transcript among the sessions and their helper agents (ANTS-6189).
- `top` (optional, default 8, 1 to 50) caps each list.
- `include_agents` (optional, default false) adds the session's helper
  agents' totals (INV-16).

## Invariants

- **INV-1** — a request is one assistant message id; lines repeating an id
  are counted once. A request's context is `input_tokens +
  cache_read_input_tokens + cache_creation_input_tokens`. `context.first`,
  `context.current` and `context.peak` are the first, last and largest of
  these. `totals` sums `input`, `cache_read`, `cache_write` and `output` over
  requests. Lines marked `isSidechain` are skipped.
  *Test:* `SessionCost.Inv1RequestsCountedOncePerMessageId`.
- **INV-2** — `largest_items` lists the biggest tool results and user or
  meta text blocks, largest first, each with `kind`, `bytes` and the
  `request` it arrived before. A tool result carries the `tool` that made
  it. Image blocks are counted in `images` and never sized by their base64
  text. *Test:* `SessionCost.Inv2LargestItemsNameTheTool`.
- **INV-3** — `startup` comes from the first `api-request-shape` line:
  `system_bytes`, `tool_bytes`, `tool_count` and `largest_tools`. A
  transcript without one says `startup: null` and still answers the rest.
  *Test:* `SessionCost.Inv3StartupFromRequestShape`.
- **INV-4** — a `session_id` outside `^[0-9A-Za-z-]{8,64}$` refuses
  `bad_args`; a folder or transcript that does not exist refuses
  `not_found`. *Test:* `SessionCost.Inv4Refusals`.
- **INV-5** — with no `session_id`, the newest transcript is read and
  `session_inferred` is true. *Test:* `SessionCost.Inv5NewestWhenUnnamed`.
- **INV-6** — the verb is registered in `registerProjectScopedVerbs` with
  `CallerCwdContract::Required`, has a `callerCwdContractFor` branch, and
  its schema sets `additionalProperties` false.
  *Test:* `SessionCost.Inv6Registration`.
- **INV-7** — in a transcript under a `subagents` folder, `isSidechain`
  lines are counted: they are that helper agent's own.
  *Test:* `SessionCost.Inv7HelperTranscriptCountsItsOwnLines`.
- **INV-8** — `session_id` `"agent-<id>"` reads that helper's transcript;
  the reply adds `parent_session_id`. No such helper refuses `not_found`.
  *Test:* `SessionCost.Inv8AgentIdReadsTheHelper`.
- **INV-9** — `session_id` `"self"` reads the newest transcript among the
  folder's sessions and helper agents, with `session_inferred` true and
  `other_active_sessions` counted over both. Every agent of a session shares
  one MCP connection, so the verb cannot name the caller; the newest
  transcript is the caller's when the caller is the one writing. With no
  `session_id`, helper agents are never read.
  *Test:* `SessionCost.Inv9SelfIsTheNewestSessionOrHelper`.

### Rent by tool, server and hook (ANTS-6697)

**Rent** is bytes held times the requests that carry them. A tool's
**server** is `<server>` in `mcp__<server>__<verb>`, and `builtin` for any
other name. An item arriving before request *p* is held from *p* to the
last request before the next compaction, or to the last request.

- **INV-10** — a request whose context is under half the previous
  request's starts a new segment: a compaction. `compactions` counts them.
  The rule is inferred from token counts: no transcript on disk on
  2026-11-07 carried an explicit compaction marker. Broken by holding an
  item past a compaction. *Test:* `SessionCost.Inv10RentStopsAtACompaction`.
- **INV-11** — `tools` lists each tool that returned a result, with
  `name`, `server`, `calls`, `result_bytes` and `rent_bytes`, largest rent
  first, capped by `top`. *Test:* `SessionCost.Inv11ToolsAndServers`.
- **INV-12** — each request rents the tool definitions of the newest
  `api-request-shape` line before it. `startup.servers` gives each server's
  `tool_count` and `bytes` in the first shape; `startup.loaded_later` gives
  the same for tools named in a later shape and absent from the first.
  *Test:* `SessionCost.Inv11ToolsAndServers`.
- **INV-13** — `attachment` lines of type `hook_success` or
  `hook_additional_context` are hook text, and `deferred_tools_delta` lines
  are the deferred-tool list, each sized by its `rendered` content; a line
  with no rendered text is skipped.
  `hooks` gives `count`, `bytes`, `rent_bytes` and `by_hook` (per
  `hookName`, capped by `top`); `deferred_tools` gives `count`, `bytes`
  and `rent_bytes`. Each added name in a deferred-tool list (its length
  plus one) rents for its server. *Test:* `SessionCost.Inv11ToolsAndServers`.
- **INV-14** — `servers` lists each server with `calls`, `result_bytes`,
  `result_rent_bytes`, `definition_rent_bytes`, `deferred_name_rent_bytes`
  and their sum `rent_bytes`, largest first, capped by `top`. `rent` sums
  `results`, `definitions`, `hooks` and `deferred_tools` into `total`.
  *Test:* `SessionCost.Inv11ToolsAndServers`.
- **INV-15** — a transcript with no `api-request-shape` line has
  `startup: null`, zero definition rent, and a `notices` entry saying so.
  *Test:* `SessionCost.Inv15NoShapeSaysWhy`.
- **INV-16** — `include_agents: true` on a session (not a helper) adds
  `agents`: the `count` of its helper transcripts and their summed
  `requests`, `totals` and `rent_total`. *Test:*
  `SessionCost.Inv16IncludeAgentsSumsHelpers`.

Label: `features;fast`.
