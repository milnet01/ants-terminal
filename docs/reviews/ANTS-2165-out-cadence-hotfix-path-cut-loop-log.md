# ANTS-2165 — Out-of-cadence hotfix path (`cut-rc.sh hotfix`) — review loop log

Record for [`../specs/ANTS-2165-out-cadence-hotfix-path-cut.md`](../specs/ANTS-2165-out-cadence-hotfix-path-cut.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 0** (draft, 2026-06-24): initial draft from the user-chosen
  "dedicated hotfix, RC rolls up" model + the ANTS-2164 cold-eyes lessons
  (exact paths, monotonicity, no `sed -i`, push-gating, conflict-safety,
  cross-spec INV disambiguation).
- **Loop 1** (2026-06-24, two lanes — verified 0C/3H/5M/4L, total 12): added
  `**Blocked by:** ANTS-2164` (reuses its unbuilt `require_no_version_drift` +
  one-RC guard); resolved the single-vs-two-phase command contradiction (§2.1
  now spells out `hotfix` phase 1 → `/bump` → `hotfix --continue` phase 2,
  matching §2.2 + §6); `N` derived from the public *tag* list, not CMakeLists
  (which step 2 moved to `R+1`); `require_no_version_drift` at publish runs on
  the `_hotfix` tree (`CMakeLists == H`), not `main` (`R+1`), so it can't
  false-trip; INV-8 cite split — `ANTS-1318 §4.4` (one-RC) vs `ANTS-2164 INV-9`
  (no-RC-for-public-base), not conflated; INV-4 full ordering `[H+1] > [H] >
  [N]`; RC roll-up flagged assistant-orchestrated + order-safe vs ANTS-2164
  INV-9; multi-SHA folds to one `[H]` entry; debian block format corrected
  (`ants-terminal (R-<rev>)`); §7 cross-refs line-pinned (ANTS-1318 INV-1
  `:157`; cut-rc.sh Subcommands `:16-22`); `fix_not_on_main` refusal code
  noted; annotated `vH` tag survives the branch prune.
- **Loop 2** (2026-06-24, two lanes — verified 0C/0H/4M/4L, total 8, 2
  dismissed): the **no-RC-in-flight** path is now explicit — INV-4 and INV-8
  branch on it (`[H+1]>[H]>[N]` with an RC, `[H]>[N]` without; INV-8 applies
  only when an RC was in flight); INV-8 test re-scoped from the unfalsifiable
  "no public tag remains" (the hotfix *creates* `vH`) to "no public `v(R+1)`,
  `v(R+1)-rc1` sole in-flight RC"; `N`-derivation rationale generalised
  (CMakeLists always tracks the RC/next-patch line, never last-public);
  abandoned-`_hotfix` idempotency added (`hotfix_branch_exists` refusal);
  INV-2 test leads with the load-bearing ancestry check; `fix_is_on_main`
  noted per-sha; ANTS-2164 §5 / ANTS-2165 step-2 composition noted. Dismissed
  unverified: a "`require_no_version_drift` warns" claim (ANTS-2164 INV-5
  `:290-292` is explicitly a HARD gate, "no warn-and-continue") and a respin
  line-cite doubt (`cut-rc.sh:234-240` confirmed the cherry-pick/abort block).
- **Loop 3** (2026-06-24, two lanes — verified 0C/2H/3M/4L, total 9, 2
  dismissed): the two-phase command is now fully implementable — Phase 2
  recomputes `H` from `_hotfix`'s `CMakeLists.txt` (post-`/bump`); Phase 1's
  `require_no_version_drift` documented as a `main`-self-consistency sanity
  check (not an `H` check); a `rc_base_mismatch` guard asserts `R == H` (the
  roll-up arithmetic's precondition); the `hotfix` subcommand + `--continue`
  flag flagged for arg-parser wiring (`cut-rc.sh:52-61`, else SHAs fall into
  `RESPIN_SHAS`); `**Blocked by:**` corrected — only `require_no_version_drift`
  is the unbuilt dependency, the one-RC guard already ships
  (`cut-rc.sh:179-185`, `ANTS-1318 §4.4`); §6 now seeds both RC-in-flight and
  no-RC fixture topologies; cut-rc.sh:159 grep quoted verbatim (`grep -vE`);
  Subcommands band `:16-23`. Dismissed: an INV-1 cite "off-by-2" (`:157` is
  correct, INV-1's bullet) and an `ANTS-1318 §4.5`-absent doubt (§4.5 exists).
- **Loop 4** (2026-06-24, two lanes — verified 0C/1H/3M/3L, total 7): `--continue`
  failure modes closed — Phase 2 now refuses a half-applied `_hotfix`
  (`CHERRY_PICK_HEAD` present) and a `vN`-no-longer-latest-public (concurrent
  promote between phases); INV-5's push-gate test scoped to the `cmd_hotfix`
  body (a `from_marker` slice, not a repo-global match a non-conforming hotfix
  could pass); the `hotfix_branch_exists` idempotency noted as a *new* guard
  (`respin` has none to copy); **Blocked by:** widened — step 2's roll-up
  `new-rc` also leans on ANTS-2164's unbuilt hardened `new-rc` (drift gate +
  INV-9 already-public-base refusal), not just `require_no_version_drift`
  (verified: `cmd_new_rc` today has only the §4.4 one-RC guard).
- **Loop 5** (2026-06-24, two lanes — both verdict **CONVERGED**, verified
  0C/0H/0M-substantive/3 polish): settled the Subcommands-band cite to `:16-22`
  and trimmed INV-4's repeated `R==H` re-derivation. No structural / mechanical
  / architectural findings — every code cite resolves, all 8 invariants are
  independently + scoped-testable, all six edge cases (no-RC, multi-SHA,
  abandoned/half-applied `_hotfix`, `rc_base_mismatch`, concurrent promote) are
  handled with named refusal codes. **Converged; spec accepted.**
