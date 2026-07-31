# ANTS-3757 cold-eyes — run state (loops owed)

**Stopped:** 2026-07-31, after loop 1, with loops still owed.
**Reason:** infrastructure, not convergence — the harness safety classifier
(`claude-sonnet-5[1m]`) went unavailable and would not authorise subagent
dispatch. Loop 2 was assembled and never ran.
**This is NOT a `--max-loops` cap exit and NOT a split stop.** Loop 2 is owed.

## Resume in one step

Everything loop 2 needs is already built and byte-stable:

| Artifact | Path |
|---|---|
| Shared context packet (brief + bounded code windows + standards passages) | `/tmp/claude-1000/-mnt-Games-Scripts-Linux-Ants-Terminal/13f97878-fce8-4b3a-9d30-88f9952b510d/scratchpad/cold-eyes-3757/shared-context.md` |
| Scrubbed doc copy (loop log replaced by the placeholder) | same dir, `ANTS-3757-roadmap-migration-read.md` |
| Loop-1 fix ledger (dispositions + sweep) | `…/scratchpad/apply-fixes-ledger.json` |

**The scrubbed copy was rebuilt from the post-fix bytes** (39,668 bytes, 0 leaked
loop-log rows), so it is current. If the scratchpad has been cleared, rebuild it
by stripping the `## Cold-eyes loop log` body to the standard placeholder — do
not hand a lane the original.

Dispatch 2 `general-purpose` lanes at the shared path. **Brief them exactly as
loop 1 — no list of what loop 1 fixed.** The two addenda that go in both prompts
verbatim:

1. Deterministic checks are settled: `spec_lint` 0 findings (603 lines),
   `doc_integrity` 0, `doc_citations` 0, 13 invariants each with exactly one
   *Breaks when*, fences balanced.
2. **Already surfaced to the user — do not report or re-confirm:**
   § 2.7's `provenance.status = "migrated"` vs `roadmap-data-model.md` § 7.7's
   "with no source-side counterpart". Live, acknowledged, pending the standard
   author's decision.

Also state that ANTS-3764 and ANTS-3765 are filed, so "does not cover the load
path" is not a defect.

## Where loop 1 left it

`Loop 1 — CRITICAL 4 · HIGH 8 · MEDIUM 9 · LOW 5 · INFO 0` (verified 26,
unverified 1). 25 fixed, 1 surfaced, 1 dismissed, 1 out-of-scope. Committed as
`01fca326`. The doc grew 404 → 603 lines, which is the number the loop-or-split
call rests on next time: still well inside the design point, but worth watching
if loop 2 adds another 200.

**Nothing is deferred.** There is no unfixed tail to fold in — the only open
item is the surfaced one above, which is a decision rather than a defect.

## Delete this file when the run converges

Its trigger is convergence, and it is keyed to a document that is still
changing. A stale resume file is worse than none.
