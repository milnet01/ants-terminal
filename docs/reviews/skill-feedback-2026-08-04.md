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

---

# Second run, same day — ANTS-3810 (round-trip oracle + acyclicity)

A separate `/write-spec` → `/doc-lint` → `/cold-eyes` run later on 2026-08-04,
on a new spec rather than a rewrite. 3 loops, 6 lanes, **66 verified, 66 fixed,
1 dismissed, no deferred tail**; CRITICALs 1 → 1 → 0. Observations below are
from that run only, and are *additional* to the four above — none of them
displaces one.

## 5. `/cold-eyes` § 1b is a free review pass, and the skill treats it as plumbing

Building the context packet means opening every citation the document makes, in
order to extract a bounded window for it. That is a review — and on this run it
was the **cheapest** one. Three of the draft's own defects died during packet
construction, before a single lane was dispatched:

- a `specs.md` § 5.5 misreading (the standard prescribes *annotation*; the spec
  claimed it permitted a bare reword),
- a cross-doc claim naming two `docs/subsystems.md` entries that do not exist,
- a `testing.md` citation for an mtime rule that file does not contain.

None is exotic; each needed one `grep`. The skill's Phase 1b is written as
context assembly ("convert every path into bounded content") with no hint that
doing it carefully *is* a pass. Worth one line in the phase, because a session
that treats it as mechanical will skim it — and skimming still produces a valid
packet, so nothing downstream notices.

## 6. The packet needs a "verified source facts" section, and § 1b never mentions one

`references/loop-carryover.md` permits carrying "settled facts about unchanged
source files" between loops. It does not say to write them down as a *packet
section*, and the payoff is larger than de-duplication:

Loop 1's lanes produced two claims that verification killed — that `source` vs
`source_path` was an inconsistency (it is the export's deliberate key naming,
with a comment in the emitter saying so) and that `mappedKind()` maps
`bug → fix` (it maps `bugfix → fix`). Both were added to the packet as verified
facts. **Neither reappeared in loops 2 or 3**, and loop 3's lanes cited the
facts back correctly. Without that section the same wrong claims cost a
verification each, every loop, forever.

Suggested shape: a `## Verified source facts` block, appended to (never rewritten
in) after each loop's Phase 3, holding only facts about files the run did not
edit. It is emphatically *not* review history — no findings, no fixes, no
severities — which is what keeps it inside the cold re-brief rule.

## 7. The `[dim N]` tally now has enough data to act on — evidence for ANTS-3814

Three loops, tagged per the brief's RULES. Aggregated:

| Dimension | Findings | Produced a CRITICAL or HIGH? |
|---|---|---|
| 2 — accuracy (doc vs code) | 11 | yes |
| 5 — completeness | 10 | yes |
| 7 — conflicts | 9 | yes |
| 4 — internal consistency | 6 | no |
| 6 — clarity | 6 | no |
| 1 — deduplication | 6 | no |
| 10 — quality discipline | 5 | yes |
| 15 — testability | 4 | yes |
| 11 — token efficiency | 2 | no |
| 13 — examples | 1 | no |
| 9 — performance discipline | 1 | no |

**Every CRITICAL and all but one HIGH came from dims 2, 5, 7, 10 and 15.**
Dims 11, 13 and 9 produced four findings between them across three loops, all
cosmetic, all fixed in one line each. That is the firing-rate-and-precision data
ANTS-3814 asks for, on a real run, and it is the first sample where the
attribution half exists.

**Do not read it as "delete dims 11/13/9" yet** — one run, one genre, one
author. It is a data point toward the four-quadrant call that bullet describes,
and the useful next step is a second run's tally to compare, not a trim.

## 8. `spec_lint`'s `sections_checked: false` is silent here — filed as ANTS-3829

Confirms `/doc-lint`'s own note at full strength. `docs/standards/specs.md`
ships no `<!-- required-sections -->` block
(`grep -c required-sections` → 0), so the `sections` check has **never run on
any spec in this project**, and `spec_lint` still returns `findings: []` with
`ok: true`. A gate that reads as clean while one of its checks is switched off
is the exact failure `/doc-lint` § SKILL.md warns about. The remedy is the
one-time project fix that skill prescribes; ANTS-3829 carries it, including the
caveat that enabling it will retro-flag existing specs.

## 9. One thing that worked, again, and should not be traded away

Same finding as (4) above, on a second sample: with bounded code windows in the
packet, **65 of 66 findings verified against source**, and the single dismissal
was a lane over-reading the packet's own roadmap enumeration as evidence a
bullet did not exist. Two runs, two samples, ~99% verification rate on a project
whose standing warning says to expect half a filed HIGH to be wrong.
