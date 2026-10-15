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
  in the last ten minutes.
- `top` (optional, default 8, 1 to 50) caps each list.

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

Label: `features;fast`.
