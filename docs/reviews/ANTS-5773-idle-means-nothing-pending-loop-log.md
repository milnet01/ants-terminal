# ANTS-5773 idle means nothing pending — review loop log

Record for [`../specs/ANTS-5773-idle-means-nothing-pending.md`](../specs/ANTS-5773-idle-means-nothing-pending.md) § Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-10-19 | 2 | 2 | 1 | 3 | 1 | 7 findings, 5 verified / 2 dismissed; all 5 fixed. Q3: the process-start rule had no route to the static per-tab parse (both lanes) — § 2.2 now pins parseIncremental's processStartMs parameter, a public finalize and the task kind enum; a flagged launch no longer reads as starting a second task. Q1: the button-and-label agreement claim deleted (the two read different transcripts); "four feature specs" corrected to three. Q4: § 7's fail-first rule exempts the two guard tests. Dismissed: the process-start finish flag and re-run timing (Q3; § 2.6 already directs, rest is local), the old 60 s tests (Q2; true, but the red suite forces the rewrite). One false clause deleted by carve-out (palette INV-9 holds no "all eight"). Six open questions and measurements resolved clean. Partition: every lane held every question. Loop 2 dispatched. |
