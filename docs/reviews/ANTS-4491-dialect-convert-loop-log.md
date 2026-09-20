# ANTS-4491 — cold-eyes loop log

## Cold-eyes loop log

| Loop | Date | Model | Lanes | Q1 | Q2 | Q3 | Q4 | Verified | Fixed | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2026-09-20 | Opus 5 | 3 | 1 | 3 | 0 | 0 | 4 | 4 | All author's own. All three lanes found § 4.6's claim that `allocationFloor()` makes the stale mirror harmless: both its terms are store-side, so on Vestige the floor is 612 while the file holds 682, and allocation would re-issue live ids. Fixed by naming the plan-side term per ANTS-3765 § 2.8 step 2. § 4.4's second-run render contradicted § 9's dialect restriction; the accepted set is now stated and INV-7 added with `dialect_out_of_scope`. § 4.3 now says what `mutate()` does beyond the column write. INV-4 narrowed from publish to commit, the commit-to-publish window being outside the guarantee. |
| 2 | 2026-09-20 | Opus 5 | 3 | 2 | 4 | 1 | 1 | 8 | 8 | VIOLENT CAP. 5 of 8 findings landed on loop-1 text - the floor rule, the accepted set, the mutate() steps and the INV-4 rewrite were all repaired again. The floor needed prefix scoping and the quarantined/synthesised exclusion, without which one foreign-prefix bullet sets it; maxAllocatedId's own comment records that class burning ~5,370 numbers. INV-4's loop-1 rewrite fixed the invariant and left a test that could not falsify it. Also: render() excludes visibility='internal', so 'the render settles them' is renderable-scoped, and a rendered id is held by § 2.6's id match, not § 2.6.1's id-less key. Cap reached; routed to implementation. |
