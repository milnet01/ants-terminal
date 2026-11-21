# ANTS-2164 — Harden the RC→release→new-RC cadence into one guarded `cut-rc.sh cycle` — review loop log

Record for [`../specs/ANTS-2164-harden-rc-release-new-rc.md`](../specs/ANTS-2164-harden-rc-release-new-rc.md) § 11. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 0** (draft, 2026-06-24): initial draft from the failure-mode
  catalogue + the live 0.7.97/0.7.98 cut analysis.
- **Loop 1** (2026-06-24, two lanes folded): §1 framing aligned to the
  user-confirmed cadence; placeholder detection pinned to the single-line
  `opens the .* preview channel` anchor (the three files' "no changes yet"
  phrasings differ — metainfo "No user-facing changes yet", debian line-wrapped);
  fragile commit-subject parse (`work_since_last_public`) replaced by
  `unreleased_has_content`; INV-2 reframed as a *new* guard, not an amendment of
  `ANTS-1318 INV-2`'s bit-identity (which INV-6 preserves); cross-spec INV refs
  qualified `ANTS-1318 INV-N`; `roll_unreleased` placeholder-replace precedence
  added; `stamp_release_date` routed through awk (not `sed -i`) with the
  `Inv4BaseReadOnly` reconciliation surfaced; RC-age boundary pinned (≥15);
  `cycle` `wednesday_guard`-once noted; §10 extended (cut-rc.sh header flags +
  test-comment change).
- **Loop 2** (2026-06-24, two lanes folded): placeholder predicates now isolate
  the version's section/block before matching the anchor (prevents a stale
  other-version placeholder false-positive); `stamp_release_date` debian target
  corrected to the RFC-2822 *trailer* (not the dateless `(X.Y.Z-1)` header);
  `rc_age_days` measured from the RC *tag creation* date, not the commit date
  (a respin re-tags an older commit); INV-7 split into benign-SKIP (nothing to
  do) vs hard-REFUSE (broken/empty/stale RC) so a broken RC can't strand the
  release; `ANTS-1318 INV-6` named in §2.4 (cycle is its permitted
  public(N)+RC(N+1) case); INV-8 states it changes INV-4's remedy flag→refuse;
  §6 "sed" wording dropped; §8 distinguishes behavioural vs source-scrape
  reproduction; §10 annotation pinned to the specs.md §5.5 inline form; respin
  no-re-roll noted (§7). Dismissed unverified: a claimed `:270-272`→`:271-273`
  off-by-one (the cite is correct — `if`=270/`echo`=271/`fi`=272).
- **Loop 3** (2026-06-24, two lanes folded — verified 0C/3H/3M/1L, 2 dismissed):
  corrected bare `debian/changelog` → `packaging/debian/changelog` and
  `metainfo` → `packaging/linux/org.ants.Terminal.metainfo.xml` (helpers would
  otherwise scan nonexistent paths and the placeholder guard pass falsely);
  INV-3 / `stamp_release_date` extended to the metainfo `date=` attribute (3
  carriers, not 2) with the RC-cut provisional-date left-state spelled out;
  `unreleased_has_content` slice + "real entry" definition pinned against the
  live empty-`[Unreleased]` layout; date-stamp + commit stated `--push`-gated
  (rehearsal mutates nothing); metainfo XML slice boundary named; INV-7
  "(exit 0)" reworded to `cycle`'s aggregate exit; §8 note that the file's
  `InvN` labels are ANTS-1318's, not this spec's. Dismissed unverified: a
  re-raised `:270-272` off-by-one (cite correct) and an ANTS-1318:351-356 cite
  doubt (cite accurate).
- **Loop 4** (2026-06-24, two lanes folded — verified 0C/2H/4M/4L, 2 dismissed):
  §2.4 `cycle` mechanism pinned — pre-checks `inflight_base` so a no-RC week
  SKIPS instead of hitting `cmd_promote`'s `exit 1`; the inter-phase base bump
  to N+1 named as a `/bump` step (cut-rc.sh never bumps — `ANTS-1318 INV-9`),
  with a new **INV-9** refusing a `new-rc` whose base is already a public tag
  (catches a forgotten bump that would cut an RC for a released version);
  build-once clarified (`promote` doesn't build, only `new-rc` does, verified
  `cut-rc.sh:260-293`); INV-3 now notes the date-stamp commit lands on `main`
  ahead of the frozen tag (tag tree keeps "— unreleased"; release body dated —
  intentional, INV-6); `roll_unreleased` "real content" cross-linked to the
  `unreleased_has_content` predicate; `release_body_is_placeholder` signposts
  CHANGELOG's separate predicate; INV-8 gloss reworded (inclusive 14-day
  boundary) + annotated-tag fixture noted; INV-7 aggregate-exit dedup; §5
  marked time-boxed reconciliation state. Dismissed: a re-raised `:270-272`
  off-by-one (correct) and a same-batch-vs-two-pushes framing nit (outcome
  accurate — INV-6 permits public(N)+RC(N+1)).
- **Loop 5** (2026-06-24, two lanes folded — verified 0C/1H/4M/3L): mechanical
  precision only — `roll_unreleased` now states it swaps only the [X.Y.Z]
  *body*, leaving the heading + "— unreleased" suffix for `stamp_release_date`;
  `stamp_release_date` debian trailer reformat pinned to `date -R -d`;
  `rc_age_days` fails CLOSED on an empty creatordate; INV-9 disambiguated from
  `ANTS-1318 INV-9`, its pre-flight ordering + orthogonality to the §4.4 guard
  stated; `cycle` exit-aggregation clarified for the promote-SKIP + new-rc mix;
  §10 ANTS-1318 annotation targets given line numbers (:160/:172). No
  structural/architectural findings — the contract is stable.
- **Loop 6** (2026-06-24, two lanes folded — verified 0C/0H-structural; both
  lanes returned "converged"): polish/accuracy only — corrected the §10 INV-2
  annotation target (`ANTS-1318-weekly-wednesday-release-cadence-patron.md:157`→`:160`; :157 is INV-1, INV-2 starts at
  :160), renumbered §2.3's Actions list (duplicate "4." → 5/6), named the
  `rc_age_days` fail-closed sentinel (9999). No structural / mechanical /
  architectural findings — **converged**; spec accepted.
