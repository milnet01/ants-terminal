# Skill feedback — session of 2026-08-04 (ANTS-3793 read-seam spec)

Collected at the user's request, to be handed to the session that maintains
`~/.claude/skills/`. Written down rather than left in a transcript because the
transcript was about to be cleared.

**What the session did**, so a maintainer can judge the sample: `/write-spec`
rewrote a 934-line umbrella spec in place as a narrowed part, `/doc-lint` gated
it author-side, and `/cold-eyes` ran three loops (6 lanes total) to its cap.
69 findings verified, 69 fixed, 1 dismissed. Every observation below is from
that run, not from recall.

---

## 1. `/cold-eyes` — the per-lane budget is stated per-turn and measured cumulative

`SKILL.md` § Budget says "~60k input tokens" per lane and requires Phase 6 to
report actual spend against it. Every one of the six lanes reported **~140–154k**
(`subagent_tokens`), which reads as a 2.5× overrun.

It is not one. The assembled packet was ~89–101 KB (~23–25k tokens) plus a
604–863-line document (~15–21k) — about **38–46k for the lane's first turn**,
inside budget. The reported figure is cumulative across the lane's three turns,
which re-send the context each time.

So the skill's own instrument says "over budget" on a run that was inside it.
Either state the budget as cumulative (~3× the per-turn figure for a 3-turn
lane), or have Phase 6 ask for first-turn input specifically. As written, a
conscientious orchestrator concludes the packet was under-built (§1b's stated
remedy) and enlarges it — the opposite of correct.

## 2. `/cold-eyes` — the fix ledger is the right idea in a shape too heavy to use

Phase 4b requires `/tmp/cold-eyes-<sid>/fix-ledger.json`, one row per verified
finding, **opened before its fix lands**, carrying `must_agree` and a
`disposition`. This session tracked dispositions inline and did not write the
file. That was a deviation, and the skill was right about the cost:

- Loop 2 split `Inv3Budgets` into `Inv3Ceiling`/`Inv3Latency` in § 6 and left
  the old name in three other sections. Both loop-3 lanes led on it.
- Loop 2 rewrote § 2.1.2's headline sentence so it contradicted a bullet 45
  lines below it. Both loop-3 lanes found it.
- Loop 2 introduced `storeFor()` inside a test clause, contradicting two bolded
  sentences loop 1 had written.

All three are exactly what a `must_agree` row catches for the price of a grep,
and all three cost a full cold dispatch instead. The evidence supports the rule.

**The friction is the shape, not the discipline.** A loop with 29 findings means
29 JSON rows authored before any edit lands, in a phase where the orchestrator
is already holding the whole finding set. A single markdown table — finding,
disposition, `must_agree` targets, verdict — would carry the same information
and would plausibly get written. Consider whether JSON is load-bearing here or
just tidy.

## 3. `/doc-lint` — the `counts` check misses the commonest count defect

`/doc-lint` returned **zero findings** on a draft that carried two real count
errors, both caught minutes later by `/write-spec` Step 4's manual "re-run every
count's command":

- "**Six** rules follow" above a list of **seven** bullets.
- "16 MiB is ~3,600 items" where the stated arithmetic gives ~3,678.

The first is the interesting one: `<number-word> <noun> follow[s]:` immediately
above a markdown list is a *greppable* shape, and the count is checkable against
the list length with no judgement at all. That is a FINDING, not a CANDIDATE.

`references/checks.md` § `counts` is currently framed around prose claims about
the codebase ("five call sites"), which genuinely need a source command. The
self-referential kind — a doc counting its own following list, table rows, or
enumerated items — is fully decidable and is the kind an author's own re-read
never catches. Worth its own check.

## 4. `/write-spec` — no branch for rewriting an existing spec into a narrowed scope

Step 1 assumes a new spec: resolve an id, pick a topic slug, check nothing
exists at that id. The actual task here was the standard post-split shape —
**rewrite an existing spec in place as one part of itself**, the parent keeping
its id, path and inbound links.

`references/drafting-rules.md` § *Splitting a spec at the cap* covers the
**parent** side (delete `RESUME.md`, open each part's log with a provenance
row). It does not cover the **part** side, which is what a later session
actually executes: does the part keep the parent's path or take a new slug; do
its invariants keep their numbers or renumber from 1; what does its first
loop-log row say.

This session had to infer all three. The corpus was ambiguous — the immediately
preceding sibling (ANTS-3808) renumbered its invariants from 1, while the split
record on the ROADMAP bullet named this part's invariants by their *umbrella*
numbers. Both are defensible; the skill should pick one. (Chosen here:
renumber, keep the path, state the mapping in § 3. Recorded so the next session
does not have to re-derive it either.)

## 5. Minor — `roadmap_query`'s truncation default

`include_body: true` truncates at 2000 chars and elides the **middle**
(`... [body elided — tail follows] ...`). On a bullet carrying a multi-step
resume plan, the plan was in the elided middle: the head and the tail both
survived and read as complete. `max_body_bytes: 16384` fixed it, at the cost of
a round-trip.

Not a skill defect, and the eliding is documented — but a bullet whose body is
a resume plan is precisely the bullet a session start reads, and the failure is
silent. Worth a note wherever the session-bootstrap guidance lives: on a
targeted `id=` fetch of a plan-bearing bullet, pass `max_body_bytes`.

---

## What the packet bought, since it is the counter-evidence to (1)

Worth recording beside the criticisms. This project's standing warning is that
cold-eyes lanes here "have twice asserted claims about code without opening the
file — expect roughly half of a filed HIGH to be wrong" (ANTS-3814).

With `/cold-eyes` § 1b's bounded code windows in the packet, **68 of 69 findings
verified against source**. The single dismissal was a lane that had routed its
own decisive question to *Open questions* rather than asserting it — the
behaviour the brief asks for, working.

Handing lanes bounded content instead of paths is doing real work. Whatever
happens to the budget wording in (1), that mechanism should not be weakened to
hit a number.
