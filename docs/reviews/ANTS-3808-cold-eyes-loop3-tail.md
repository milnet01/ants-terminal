# ANTS-3808 — cold-eyes loop 3 (cap): verified findings, unfixed

> **CLOSED 2026-08-04 — all four folded in, plus the § 4 decision. Nothing here
> is outstanding.** Two findings did not survive verification against source and
> were dismissed; the record of each is below, appended in place. Disposition:
>
> | | Disposition |
> |---|---|
> | **T1** (HIGH) | **Split.** Match-failure half **fixed** — the strip no longer matches text at all; the reader records a `BulletRecord::headlineEnd` offset and the migration cuts there (§ 2.1, user-accepted). Truncation half **dismissed**, see below |
> | **T2** (HIGH) | **Fixed** — INV-1 now counts each trailer key's *canonical value* exactly once, not the key literal, with the two-value counter-example stated and propagated to its *Test:* clause |
> | **T3** (MEDIUM) | **Dismissed** — not reachable, see below |
> | **T4** (MEDIUM) | **Fixed** — `makeItem()` (`src/roadmapmigrate.cpp`) named as the owning seam in § 2.1 and § 4; the `PlannedItem.headline` → `ItemWrite.headline` identity asserted against `src/roadmapmigrateload.cpp`'s `w.headline = it.headline` |
> | **§ 4 build decision** | **Settled by the user** — hoist `roadmapparse.cpp` + `roadmapindex.cpp` into a `Qt6::Core`-only `ants_roadmapparse_lib` that both `ants_core_lib` and `ants_roadmapstore_lib` link. No new files |
>
> **Dismissed on verification, with the evidence:**
>
> - **T1's second gap ("the string may not be the untruncated one").** The
>   premise is false. The tail claimed `roadmapparse.cpp` "has an em-dash/INV-4
>   path that sets `rec.headline` directly and skips `assignHeadline()`". It does
>   not — that path *calls* `assignHeadline()`, and the
>   `if (!rec.headline.isEmpty())` line the claim rests on is a **guard** testing
>   whether the em-dash branch already ran, not an assignment. All four headline
>   sites go through `assignHeadline()`, which sets `headlineFull` and `headline`
>   together, so `makeItem()`'s ternary always yields the untruncated form. Moot
>   in any case under the accepted fix: nothing is matched.
> - **T3 (GFM task-list bullets).** Not reachable. The reader strips the
>   checkbox from `head` (`head.remove(0, 3)` plus a leading-space loop) on the
>   GFM branch, and `body` is seeded from `head` **after** that — so a `[ ]` /
>   `[x]` token never reaches `body`'s first line, which is what § 2.1's strip
>   operates on. Reinforced by the reader's own note that the body-wide `rxId`
>   matches only a dashed `[PROJ-NNNN]`. No table row added; § 2.1 records the
>   dismissal so it is not re-raised.

**These are verified and unfixed. Do NOT re-review to rediscover them** — a
fresh loop costs a full two-lane dispatch (~235k subagent tokens) to regenerate
what is already written here. **Fold them in directly.**

Subject: [`docs/specs/ANTS-3808-item-body-and-trailer-suppression.md`](../specs/ANTS-3808-item-body-and-trailer-suppression.md).
Run stopped at `--max-loops 3`. Loop 3: 23 verified, 19 fixed, **4 filed here**,
2 dismissed.

**Why these four and not the other nineteen.** All four need a decision rather
than a correction, and every one of them lands in § 2.1 or § 2.3.1 — the two
sections whose repairs generated most of loops 2 and 3's findings. Rewriting
them a third time at the cap is the move Phase 5's stop-and-consolidate trigger
exists to prevent.

---

## HIGH

**T1 — § 2.1 step 1 defines no behaviour when the headline string is not found
in `body`'s first line.**

Step 1 says to remove "the headline text — matched as the exact string
`ItemWrite.headline` will carry". Two gaps, both reachable:

- **The match can fail.** A headline the reader normalised (de-markup via
  `h.remove("**")`, trailing-caret-anchor strip, `trimmed()`) is not
  byte-identical to the source line it came from. On failure the head line
  survives intact and the duplication INV-1 forbids returns **silently** — the
  exact defect this spec exists to remove, restored by its own repair.
- **The string may not be the untruncated one.** § 2.1 asserts
  `ItemWrite.headline` is untruncated, quoting `makeItem()`'s
  `rec.headlineFull.isEmpty() ? rec.headline : rec.headlineFull`. That ternary's
  *first* branch yields `rec.headline`, which is truncated to 120 chars with an
  ellipsis. `assignHeadline()` sets both fields together so the branch is
  normally unreachable — **but `roadmapparse.cpp` has an em-dash/INV-4 path that
  sets `rec.headline` directly and skips `assignHeadline()`**, leaving
  `headlineFull` empty. Whether that path can reach migration is unverified.

**Decide:** anchor the match to the leading occurrence only; state the fallback
(leave the line intact, or refuse the migration with a note); and either confirm
the em-dash path cannot reach `makeItem()` or handle the truncated case.
**Do not** fix this by loosening the match — a fuzzy prefix strip is how prose
starts disappearing again.

**T2 — INV-1 and § 2.3.1 disagree about what "exactly once" counts.**

INV-1 asserts "each trailer key it carries exactly once", and its test says
"exactly one of each **trailer key** in the rendered text". § 2.3.1's closing
says "Both branches land on exactly one occurrence of the **canonical value**,
which is what INV-1 and INV-3 actually assert."

These are different assertions and they disagree on a state § 2.3.1 calls
correct: in the no-suppression branch the residual carries `Source: B` and the
column emits `Source: A.`, so the **key literal appears twice** while the
canonical value appears once. A test author following INV-1's wording writes a
case that fails on legal output; one following § 2.3.1 writes a weaker test than
INV-1 promises.

**Decide** which INV-1 asserts, then propagate to its *Test:* clause. The
value-counting reading is probably right — it is what § 2.3's suppression
actually protects — but it makes INV-1 strictly weaker than its current wording,
and that is a contract change, not an edit.

## MEDIUM

**T3 — GFM task-list bullets are unhandled by § 2.1's strip.**

Step 1 removes "the leading `[<id>]` token **if present**". A GFM task-list
bullet — `- [ ] text` / `- [x] text`, a format `detectRoadmapFormat()` detects
and `parseBullets()` supports — puts a bracket token at the head of the line
that is **not** an id. § 2.1's table has a GFM row, but it describes the
plain-GFM case where the headline is the whole line, not the task-list case.

Unstated whether `[ ]` is matched by the id rule. If it is, a real id could be
mis-stripped; if it is not, every GFM task-list bullet keeps a `[x]` residual
that the render then re-emits below its own head.

**Decide:** restrict the id rule to `[PROJ-NNNN]`-shaped tokens, and add a
task-list row to § 2.1's table with its own INV-5 fixture.

**T4 — § 2.1 never names the function that performs the strip, and relies on an
identity it never asserts.**

§ 1 names `makeItem()` as the copier, but `makeItem()` builds a `PlannedItem`
while § 2.1's rule is stated against `ItemWrite.headline`. The spec never says
those two strings are the same value, and never says which seam does the strip —
`makeItem()` in `src/roadmapmigrate.cpp` (`ants_core_lib`, no database) or
`RoadmapMigrateLoad::load()` in `src/roadmapmigrateload.cpp`
(`ants_roadmapstore_lib`). An implementer must guess, and the two live in
different libraries, so the guess interacts with § 4's open build decision.

**Decide:** name the owning function, and assert the
`PlannedItem.headline` → `ItemWrite.headline` identity the strip depends on.

---

## Dismissed on verification (do not re-raise)

- **"§ 2.3.1's non-suppression branch breaks INV-3 on a purely migrated item"**
  (raised CRITICAL, loop 3 lane A). The scenario requires a trailer key inside
  the bold headline to vanish from the rendered output — but § 2.4's render
  **reconstructs the headline into the head line**, so the key reappears there,
  ahead of the residual, and a re-parse takes the same first match the original
  parse took. Round-trips. No divergence could be constructed.
- **"`test_core` is not verified to be the *only* bundle linking both
  libraries"** (raised as an open question in all three loops). Verified TRUE at
  loop 1: `ants_roadmapstore_lib` appears in exactly one bundle,
  `CMakeLists.txt:2128`.

## Still surfaced to the decision-maker, not filed here

**§ 4's build decision** — § 2.3 places a `RoadmapParse::trailerValuesIn()` call
inside `ants_roadmapstore_lib`, which links `Qt6::Core` + `Qt6::Sql` and nothing
else by deliberate design (`src/roadmaprender.h:11-12`, for ANTS-3794's headless
publish path). § 4 carries three options and a named owner; it is a decision, not
a defect, and it is a **precondition of implementing § 2.3**.
