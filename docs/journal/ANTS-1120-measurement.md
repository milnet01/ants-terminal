# ANTS-1120 — Token measurement, 2026-10-15

**Status:** measured; verdict inconclusive (user, 2026-10-15).

## What was measured

The SIGN-1 bench in [`tools/token-bench/`](../../tools/token-bench/README.md),
as the user decided on 2026-10-15: one fixed prompt, Claude Code in Ants
against Claude Code with Ants taken out. This replaces the spec's
stub-based configurations.

- **Job:** fix the planted bug in `tally`. Prompt and model are in the
  bench README: `claude-opus-5-5`, effort `high`.
- **Arms:** `ants` has the Ants MCP server and Ants hooks; `plain` has
  neither. `bench.py`'s header lists the rest.
- **Runs:** four per arm, alternating, all on 2026-10-15. Every run fixed
  the bug.
- **Counts:** each assistant message's API `usage` field in the session
  transcript. Cost is those counts at the published Opus 5.5 price. Each
  run's cost equals Claude Code's own `total_cost_usd` for it.
- **Raw data:** [`ANTS-1120-results.jsonl`](ANTS-1120-results.jsonl), one
  line per run. `tools/token-bench/bench.py summary` reprints the tables.

## Per run

Tokens by type. No run wrote to the 5-minute cache.

| Arm | Started | Input | Cache write (1h) | Cache read | Output | Cost (USD) |
|---|---|---|---|---|---|---|
| ants | 09:50:32 | 12 | 32,565 | 145,245 | 1,940 | 0.3284 |
| plain | 09:51:00 | 12 | 15,519 | 126,616 | 1,928 | 0.1881 |
| ants | 09:51:28 | 12 | 13,945 | 145,830 | 1,757 | 0.1759 |
| plain | 09:51:56 | 10 | 15,052 | 100,399 | 1,944 | 0.1794 |
| ants | 09:52:23 | 12 | 16,265 | 157,084 | 1,867 | 0.1989 |
| plain | 09:52:50 | 14 | 18,026 | 157,418 | 1,970 | 0.2151 |
| ants | 09:54:01 | 12 | 16,208 | 156,997 | 1,851 | 0.1981 |
| plain | 09:54:32 | 12 | 18,939 | 134,021 | 2,046 | 0.2193 |

## Totals

| | Ants mean (sd) | Plain mean (sd) | Ants against plain |
|---|---|---|---|
| All runs, 4 each | 0.2253 (0.0695) | 0.2005 (0.0197) | 12.4% more |
| Warm-cache runs | 0.1910 (0.0131), 3 runs | 0.2005 (0.0197), 4 runs | 4.7% less |

Ratio plain/ants over all runs: 0.890. Conservative ratio
(plain − 1 sd) / (ants + 1 sd): 0.613.

SIGN-1 asks for Ants to be at least 20% cheaper. **Neither view meets it.**

## What explains the numbers

- **The first Ants run started on a cold cache.** Its first request wrote
  23,719 tokens and read none. Every other run's first request read the
  shared prompt from the cache. That one run is why "all runs" shows Ants
  dearer; "warm-cache runs" leaves it out.
- **Ants adds about 5,250 tokens to every request.** A run's first request
  holds 23,718 tokens in the Ants arm and 18,462 in the plain arm. That is
  the Ants tool list and the context Ants hooks add.
- **No run called an Ants tool.** Each run finished in five to seven
  requests: run the tests, read a file, fix it, run the tests again. The job
  is too small to need code search, so Ants' tools had nothing to save.

## Against the spec

- INV-2 (three or more runs per arm, same prompt): met, four each.
- INV-3 (counts from API `usage`): met.
- INV-4 (point and conservative ratios): met, above.
- INV-1 (the task exercises the audit, roadmap-query and ID surfaces): not
  met. The SIGN-1 job touches none of them.
- INV-5 (the user's verdict): met, below.
- INV-6 (the verdict's roadmap edit): ANTS-1120 is annotated "needs deeper
  measurement" and stays open.

## Verdict (user, 2026-10-15)

- **Inconclusive.** SIGN-1 is not met, but this bench never reaches a tool
  Ants adds, so it measures only Ants' fixed overhead.
- **Threshold:** SIGN-1's own 20%, unchanged.
- **Next:** a second, larger bench whose job needs code search, measured
  the same way. The first bench stays as it is.
