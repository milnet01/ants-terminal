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

## Bench 2: pygments (ANTS-5697), 2026-10-15

The second bench plants a bug in Pygments 2.21.0, a 343-file library
([`tools/token-bench/pygments/`](../../tools/token-bench/pygments/README.md)).
Four runs per arm, alternating; every run fixed the bug. Raw data:
[`ANTS-1120-results-pygments.jsonl`](ANTS-1120-results-pygments.jsonl).

| Arm | Started | Input | Cache write (1h) | Cache read | Output | Cost (USD) |
|---|---|---|---|---|---|---|
| ants | 10:27:24 | 12 | 12,392 | 141,410 | 1,851 | 0.1645 |
| plain | 10:29:50 | 8 | 10,373 | 68,425 | 1,228 | 0.1213 |
| ants | 10:30:47 | 8 | 11,527 | 89,344 | 1,163 | 0.1334 |
| plain | 10:31:48 | 8 | 10,698 | 69,278 | 1,200 | 0.1235 |
| ants | 10:32:59 | 10 | 11,425 | 114,422 | 1,456 | 0.1434 |
| plain | 10:33:45 | 8 | 10,382 | 69,261 | 1,128 | 0.1195 |
| ants | 10:34:38 | 8 | 10,670 | 88,155 | 969 | 0.1224 |
| plain | 10:35:29 | 6 | 10,805 | 49,737 | 993 | 0.1163 |

- Ants mean 0.1409 (sd 0.0179), plain mean 0.1202 (sd 0.0030): **Ants
  costs 17.3% more.** Ratio plain/ants 0.853; conservative ratio 0.738.
- Every run's first request read the shared prompt from the cache, so no
  run started cold.
- **No run called an Ants tool again.** Each found the bug with two to
  five shell commands in three to six requests: run the tests, one text
  search, a one-line fix. A larger codebase did not make the job need
  code search; a failing test plus one search found it.

What the two benches show together: on a short task Ants costs its fixed
overhead (about 5,250 tokens a request) and saves nothing, because its
tools go unused. Where Ants' tools might pay is the long session: the
2026-10-15 token survey measured 100 to 274 requests per working
session, where context rent, not a single fix, dominates the cost. A
bench of one fixed prompt does not reach that.

## Verdict (user, 2026-10-15)

- **Inconclusive.** SIGN-1 is not met, but this bench never reaches a tool
  Ants adds, so it measures only Ants' fixed overhead.
- **Threshold:** SIGN-1's own 20%, unchanged.
- **Next:** a second, larger bench whose job needs code search, measured
  the same way. The first bench stays as it is.
