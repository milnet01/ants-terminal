# ANTS-1318 — Weekly Wednesday release cadence + Patron-tier frozen-RC pipeline — review loop log

Record for [`../specs/ANTS-1318-weekly-wednesday-release-cadence-patron.md`](../specs/ANTS-1318-weekly-wednesday-release-cadence-patron.md) § 12. Cold-eyes review log.

## Cold-eyes loop log

- **Loop 0** (initial draft, 2026-05-13 from first subagent's
  frozen-RC content + user clarification on 2026-05-13 that
  cherry-pick + tag-based is the right model).
- **Loop 1** (2026-05-21): `/release` references updated to
  `cut-rc.sh` throughout; `--force-ad-hoc` → `--force-non-wed`;
  status updated to reflect `cut-rc.sh` implemented 2026-05-20.
- **Loop 2** (2026-05-21): §2.4 step 9 rewritten — spec said
  "two tags batched in one push" but implementation is two sequential
  `cut-rc.sh` invocations; INV-2 non-fatal promote check noted;
  §5.3 "Wed dual-cut" phrasing updated.
- **Loop 3** (2026-05-21): **Kind:** field added; §2.5 rewritten in
  past tense to describe the shipped mechanism (RC tag as its own
  zsync channel, not `v*-rc` wildcard or `rc-channel` tag); §11 step 3
  marked ✅; OQ-4 line reference 199–204 → 216–231; §11 step 6
  emoji 📋 → ✅ updated to 🚧 → ✅; `release.yml:172` → `:189`
  throughout; §10 Tests section added; all INVs note `*Test:*`
  coverage (see §10).
- **Loop 4** (2026-05-21): Step 0 table `--force-non-wed` behavior
  corrected (warns + continues, not required); INV-3/INV-5 *Test:*
  clauses updated to "source-scrapes" (not live-run); INV-7 *Test:*
  replaced false claim with "no automated test"; §9 Tests renamed →
  §10, wrong INV-7 coverage claim removed; §10 → §11, §11 → §12
  renumbering.
- **Loop 5** (2026-05-21): Header "See §11" → "See §12"; INV-9
  label "patron notification template" → "bump.json schema"; OQ-4
  marked resolved in §8 and §9 (implemented in `release.yml:216–231`);
  INV-4 and INV-6 added to process/manual list in §10; §11 step 4
  marked ✅; stale loop-3 log references corrected.
- **Loop 6** (2026-05-21): §1 tag count "4 patch tags" → "5 patch
  tags" + verification command updated to `git for-each-ref`; INV-2
  mechanical note corrected (`git diff --quiet "${rctag}"` is working
  tree vs RC tag, not a two-tag diff; warns and continues, no
  interactive confirmation); §4.4 "stops and asks" → "prints an error
  and exits (exit 1)"; §5.1 "line 33" CLAUDE.md citation removed;
  §6 `new-rc --push` now notes it also pushes `origin main`; §10
  test coverage: Inv5OneRcInFlightGuard + Inv6PushGated added; local
  vs spec INV numbering mismatch noted; §2.2 memory file link
  hyphens → underscores, broken bracket-link removed; §2.1
  check-version-drift.sh description corrected to two-grep chain;
  §2.5 "The branch" → "The conditional" in the mitigation bullets.
