# ANTS-3446 — `feedback_log op:"migrate_v2"`: one-shot mechanical v1→v2 feedback-file migration — review loop log

Record for [`../specs/ANTS-3446-feedback-log-op-migrate-v2.md`](../specs/ANTS-3446-feedback-log-op-migrate-v2.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two-phase review: 6 loops on the original **relocate-tables-to-an-archive**
design, a mid-review **design pivot** to **leave-tables-in-place**, then 3 loops
on the rewritten design. All loops 2 reviewers, cold (loop N+1 briefed
identically to loop 1 — never told what was fixed).

### Phase A — relocate-tables design (loops 1–6)

- **Loop 1** — 0C, 2H, 4M, 4L. H: the finding-shaped bullet regex required
  `**What**` immediately (never matched the canonical `**What:**`); §7's
  standard-reconciliation list was incomplete (3 unflagged auto-stamp sites).
- **Loop 2** — **1C**, 1H, 3M, 2L. C (both lanes): the table-relocation extent
  used `scanBoundaries`'s `#`/`## ` set, which is `### `-blind, so it swept a
  trailing `### ` finding into the archive — the §2.8 example was un-reproducible.
  Fixed by making the extent `### `-aware (union with `enumerateFindingBlocks`).
- **Loop 3** — 0C, 2H, 3M, 5L. Blank-line reproducibility of the relocation;
  §4/§7 still named `scanBoundaries` for extents (contradicting the loop-2 fix).
- **Loop 4** — 0C, 0H, 3M, 6L. "parse exposes the boundaries via its result" was
  false (`ParseResult` has no boundary list); marker malformed/mis-placed edges.
- **Loop 5** — 0C, 0H, 1M, 5L. §7 kept missing scattered stale `migrate_v2`
  provenance markers → replaced item-by-item enumeration with a repo-wide
  provenance-sweep instruction (complete-by-construction).
- **Loop 6** — 0C, 0H, 2M, 3L. **The load-bearing finding:** relocating the
  tables to EOF moves the v1 watermark (`lastMaintainerLine`) to EOF, which
  **empties the shipped v1 un-triaged delta** on a migrated file until the
  position-independent v2 reader ships — a silent regression window. Documented as
  an ordering dependency, then surfaced to the maintainer as a design fork.

### Design pivot (2026-07-05)

The maintainer chose **leave-tables-in-place** over relocate-plus-dependency: it
removes the delta-dark hazard entirely (watermark unmoved → v1 delta preserved),
and drops the extent-computation, blank-line-normalisation, and archive machinery
— which had generated most of the phase-A findings (the loop-2 CRITICAL, the
loop-3 blank-reproducibility, the loop-6 delta hazard). The spec was rewritten to
two mechanical passes (marker bump + stamp), carrying forward every verified
phase-A learning (the `[:*]` bullet regex, the finding-shaped discriminator, the
marker `[0-9]*`/≥2 handling, orphans[]/unclassified[], the §7 provenance sweep,
the refusal set) minus relocation.

### Phase B — leave-in-place rewrite (loops 1–3)

- **Loop 1** — 0C, 1H, 2M, 3L. H (both lanes): the §2.8 envelope line numbers were
  miscounted (Issue #1 12→13, Positive note 19→16 — the numbers a conformance test
  copies). Plus a stale `migrate_v2` attribution in the sibling ANTS-3443 spec.
- **Loop 2** — 0C, 1H, 2M, 3L. H (both lanes): §2.8 narrated the v1 delta as
  "everything after the last maintainer heading" and claimed it returned the
  post-watermark `### ` findings — but the delta is `#`/`## `-keyed (`### ` inert),
  so the minimal fixture's delta is empty. Reworded the delta model accurately and
  stated the load-bearing **marker-blind** premise (shipped readers call `parse()`
  unconditionally, so the `: 2` bump does not route to the unbuilt v2 reader).
- **Loop 3** — **0C, 0H, 0M** (both lanes) — **clean.** Every load-bearing claim
  re-verified live: marker-blind at `remotecontrol.cpp:10748`/`13928`; delta-start
  = first `#`/`## ` after the watermark (`### ` inert, `isBoundaryHeading`);
  `maintainerAnchorRe`, `enumerateFindingBlocks`/`idLine0`, `renderFindingBlock`
  placeholder, `ParseResult`-has-no-version all confirmed; §2.8 line numbers
  byte-exact; §7 ten-item reconciliation complete against the standard; ANTS-3443
  reconciled. Residual: LOW/INFO polish only (a surviving "pure v2" line in the
  standard folded into §7 item 1; blank-first stamp insertion pinned; the
  heading-arm prose false-positive noted; test-coverage nits) — all applied. No
  structural/mechanical/architectural finding survived.

- **Convergence (2026-07-05):** 9 loops across two designs. The pivot to
  leave-in-place was the decisive simplification — it dissolved the phase-A
  CRITICAL and the delta-dark hazard rather than patching them. Phase B converged
  in 3 loops (clean at loop 3 modulo polish). Signed off as implementation-ready;
  the only cross-doc commitments the implementer must honour are the §7 standard
  reconciliation (ten items) and the shared `enumerateFindingBlocks` scanner
  (§2.4/§7).

### Phase C — ANTS-3475 backfill reconciliation review (2026-07-13, loops 1–4)

Cold-eyes re-review after amending the spec for the shipped ANTS-3474 opt-in
`backfill_from_tracking` pass (matcher §2.2.1, INV-12, `backfilled[]` envelope,
§2.1 request row) and its consistency tails. 2 reviewers/loop, cold (loop N+1
briefed identically to loop 1 — no prior-fix list). Every finding verified against
current `feedbackfile.cpp`/`.h`, `remotecontrol.cpp`, `claudeintegration.cpp` and
the shipped conformance test before fixing.

- **Loop 1** — 0C, 0H, 2M, 4L. §2.7/INV-10 mis-attributed version extraction to
  migrate's own scan and claimed `ParseResult` has no version field (it has
  `formatVersion`, ANTS-3448 — verified `feedbackfile.h:73`); §6 coverage capped at
  INV-11; §2.2/§1.1 unscoped "does not shrink"; §2.5 order-of-ops omitted the
  backfill pass; §5/§7 called `assign_id` "pending" (shipped ANTS-3447). All fixed.
- **Loop 2** — **0C, 1H, 1M, 3L.** The cold re-read surfaced a pre-existing
  **ANTS-3448 drift** the amendment hadn't touched: §1.1/§2.4/§2.8 asserted a
  present-tense **marker-blind** reader and **INV-4** claimed
  `parse(after).delta == parse(before).delta` — both false since `parse()` became
  marker-aware (`if (v2)` branch), and contradicted by the shipped `V1DeltaPreserved`
  test (which asserts `after.formatVersion == 2` + the stamp in `after.delta`, NOT
  delta identity). Re-tensed the marker-blind narrative to history; recast INV-4 to
  **watermark preservation, not delta identity**; swept the stale "unbuilt/pending
  v2 reader" refs (the v2 `feedback_query` delta shipped and now shares
  `bulletArmRe()` — shared *code*, not just definition).
- **Loop 3** — 0C, 0H, 2M, 2L, 1I. §7 items 4/8 still asserted (unscoped) that
  migrate "never reads a table's id content" (backfill reads `trackingRows`) → scoped
  to the default path; §2.4 orphan bullet said a bullet-bearing above-watermark
  orphan "becomes prose under v2" (the v2 scan is position-agnostic → it surfaces as
  `suspected_untagged`) → qualified; §5 relocate bullet re-tensed; the §2.4 heading-arm
  over-stamp example used the plural "Observations summary" which `\bobservation\b`
  does **not** match → switched to a singular example + noted the plural is not
  over-stamped; INV-11 "only way negative" widened to cover multi-digit/leading-zero
  markers. All applied.
- **Loop 4** — **0C, 0H, 0M, 2L** (both lanes) — **polish-converged.** Every
  backfill/matcher/marker-aware/envelope/version claim re-verified clean against the
  code and the 18-test suite. Sole residual: §6 over-described its own test coverage
  — the "non-canonical anchor" and "above-watermark prose unreported" cases have no
  shipped fixture (grep-verified 0 hits). §6 reworded to the real coverage; the two
  genuine test gaps roadmapped as **ANTS-3508** (test). No structural / mechanical /
  architectural finding survived.

- **Convergence (2026-07-13):** 4 loops. The decisive catch was loop 2's
  marker-blind/INV-4 drift — a latent ANTS-3448 staleness orthogonal to the backfill
  amendment, caught only because loop 2 re-read cold. Phase C fixed 3 mandated
  ANTS-3475 points + the ANTS-3448 delta-reader reconciliation + assorted consistency
  tails, and left one roadmapped test-coverage gap (ANTS-3508).
