# Recording a whole triage in one write — ANTS-4671

**Status:** implemented (2026-08-26)

## Problem

`feedback_log op:"assign_id"` fills ONE finding's `**Proposed ID:**`
slot per call. A triage decides many findings at once — they are read
together via `feedback_query`, judged together against one dedup sweep,
and several legitimately share an id. Writing that decision down then
cost one full read and one atomic write of a file in the tens of
kilobytes, per finding.

The same argument was accepted three times on the roadmap side —
`flip_batch` (ANTS-1690), `annotate_batch` (ANTS-4470) and
`amend_batch`'s item — and the feedback side had no batch op at all.
The shape is stronger here, because a triage is *inherently* a batch:
nothing between the calls can have changed the file, which is exactly
where a per-call read and write is least justified.

## Contract

**`op:"assign_id_batch"` takes `assignments[]`, one read, one atomic
write.** Each element takes `assign_id`'s existing per-call arguments
UNCHANGED — `heading`, optional `heading_line`, exactly one of `ids` /
`closure` / `awaiting`, optional `note` — so there is no second argument
grammar to learn.

**Composition is shared, not re-implemented.** `fbComposeAssignValue`
holds the exactly-one-of rule, the `^ANTS-[0-9]+$` gate with
first-occurrence de-duplication, and the control-character folds. Both
ops call it, so they cannot answer one assignment differently.

**Per-assignment failures cost only their own assignment.** They land in
`skipped[]` carrying their `index` and the same refusal envelope the
single op returns. This matters more here than elsewhere: a heading is
matched verbatim, so one mistyped heading must not cost the batch its
other closures.

**An all-failed batch refuses.** A caller reading only `ok` must not
read "every heading was mistyped" as a completed triage — ANTS-4470's
rule, kept.

**Assignments are threaded.** Each sees the previous one's result, so
two aimed at one finding behave exactly as two separate calls would.

## Invariants

- **INV-1** — N assignments apply in one call and all land in the file.
  *Test:* `Inv1AppliesAll`.
- **INV-2** — an unmatched heading lands in `skipped[]` with its
  `index`, and the other assignments still apply. *Test:*
  `Inv2OneBadHeadingDoesNotCostTheRest`.
- **INV-3** — a batch where every assignment fails refuses, rather than
  reporting success with nothing applied. *Test:* `Inv3AllFailedRefuses`.
- **INV-4** — `dry_run:true` writes nothing to the file. *Test:*
  `Inv4DryRunWritesNothing`.
- **INV-5** — two assignments to one finding behave as two sequential
  calls: the second wins. *Test:* `Inv5AssignmentsAreThreaded`.
- **INV-6** — an empty or absent `assignments` array refuses `bad_args`.
  *Test:* `Inv6EmptyAssignmentsRefused`.

## Out of scope

- The single `op:"assign_id"`, which is unchanged in behaviour and is
  covered by `tests/features/feedback_log_assign_id/`. Its composition
  now routes through the shared helper, which those tests still cover.
