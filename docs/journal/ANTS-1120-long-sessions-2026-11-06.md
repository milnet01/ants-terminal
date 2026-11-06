# ANTS-1120 — Ants cost in past long sessions, 2026-11-06

**Status:** measured. SIGN-1 (Ants at least 20% cheaper) is not met.

The user decided on 2026-11-01 that this step reads long sessions already on
disk with `session_cost` and reports what Ants costs and saves, with no new
runs. Earlier benches: [`ANTS-1120-measurement.md`](ANTS-1120-measurement.md).

**This is not a controlled comparison.** The sessions are different tasks in
different projects, and every one ran with Ants installed. None is a no-Ants
baseline.

## What was read

Twelve transcripts under `~/.claude/projects/`, each of 100 or more requests,
across eight projects: five with few Ants calls and seven with many. Requests,
context, token totals and start-up tool bytes come from `session_cost`. Ants
call counts and result sizes per tool come from a script over the same
transcripts, because `session_cost` has no per-tool totals. A helper agent did
the read. Two sessions were re-checked with `session_cost` and matched:
`Ants_Terminal/3e2dc307-183d-4d5a-845d-27c961c94547` (431 requests, 145.5M
tokens read from cache) and `RetroArch/f574b098-76ad-4fc9-aac0-9b3c4f5769e8`
(140 requests, 27.6M).

| Session | Requests | Cache read | Ants calls | Ants rent, % of cache read |
|---|---|---|---|---|
| RetroArch/f574b098 | 140 | 27.6M | 3 | 2.0% |
| OneUp/c6a677ee | 146 | 31.5M | 4 | 4.3% |
| RetroDB/4c24a18f | 262 | 72.6M | 10 | 2.0% |
| Album_Builder/f29413cc | 240 | 63.9M | 10 | 3.8% |
| RetroDB/875320e4 | 610 | 253.8M | 61 | 2.3% |
| demoreel/87e44ce0 | 562 | 183.1M | 61 | 2.3% |
| Vestige/612b4c4a | 498 | 181.2M | 47 | 2.7% |
| Ants_Terminal/d0c98ac7 | 222 | 54.7M | 93 | 6.0% |
| OneUp/32ea1c1e | 101 | 24.3M | 74 | 4.2% |
| LocalWebServerManager/27fc4338 | 117 | 19.2M | 53 | 6.2% |
| Ants_Terminal/6a13361d | 104 | 28.6M | 133 | 7.2% |
| Ants_Terminal/3e2dc307 | 431 | 145.5M | 116 | 4.3% |
| **All twelve** | | **1,086M** | | **3.2%** |

## Cost

**Rent** is the Ants text every request carries again: tool definitions in
that request, Ants hook text, and Ants names in the deferred-tool list. It is
summed per request until a compaction drops it. Bytes become tokens at 4 bytes
per token, an assumption; at 3 bytes per token the rent is a third higher.

Rent is 2.0% to 7.2% of each session's cache reads, 3.2% over all twelve. Claude
Code now loads four or five Ants tools at start (about 7.5 to 11 KB) and the
rest through ToolSearch, so the per-request overhead is below the benches'
5,250 tokens.

## Saving

**No saving is backed by a number.** Read and search verbs were compared
with the plain tools they stand in for: `read_region`, `read_regions` and
`file_outline` against Read and `cat`/`sed`; `workspace_search`,
`find_definition`, `trace`, `find_caller` and `find_sources` against Grep,
Glob and `grep`/`rg`/`find` in Bash. The Ants search verbs returned 1.7 to
3.0 KB per call against about 0.9 to 1.3 KB for grep-style calls, and the read
verbs were usually larger than Read too. One session of twelve showed a smaller
Ants result per call.

A transcript cannot show whether one Ants call replaced several plain ones,
and so saved requests. Only a controlled bench could. `roadmap_query` and
`roadmap_log` have no plain equivalent and count as neither cost nor saving.

## Gaps in `session_cost` this read hit

No per-tool totals; start-up shows only the first request's tools, with no
per-server subtotal and nothing loaded later; no figure for hook text, rent or
compaction; older transcripts give no start-up at all; helper-agent
transcripts are not added to their session. Filed as ANTS-6697.
