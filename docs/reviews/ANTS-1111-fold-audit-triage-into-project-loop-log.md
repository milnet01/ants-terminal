# ANTS-1111 — Fold `/audit` triage into the Project Audit tool — review loop log

Record for [`../specs/ANTS-1111-fold-audit-triage-into-project.md`](../specs/ANTS-1111-fold-audit-triage-into-project.md) § 11. Cold-eyes audit trail.

## Cold-eyes loop log

### Loop 1 (2026-05-13)

One reviewer dispatched against this spec + ADR-0003 pair.
Findings raised + resolutions applied:

- **HIGH-1: § 3.5 vs § 3.8 misroute (3 sites).** roadmap-format.md
  § 3.5 is "Bullet structure" (per-bullet contract); § 3.8 is
  "Findings fold-in subsections" (block contract). The spec
  conflated the two. **Resolved:** § 2.4 doc comment for
  `templateRoadmapFoldInBlock` now cites both — § 3.8 for the
  subsection shape, § 3.5 for the per-bullet field set.
- **HIGH-2: insertBlock placement algorithm wrong.** "Before the
  first `## ` heading" would put the block above the
  Distribution-adoption overview / TOC sections — far from any
  release block, violating roadmap-format.md § 3.8 ("inside the
  active release block"). The current ROADMAP has no `[Unreleased]`
  convention; release blocks are named `## 0.7.X — theme — shipped
  DATE`. **Resolved (loop 1):** § 2.5 algorithm rewritten to use a
  new `## [Unreleased]` convention (mirroring CHANGELOG); helper
  inserts under that heading, creating it if absent. INV-8
  rewritten to lock both cases (a) and (b) on fixture ROADMAPs.
  *(Superseded by loop-2 CRITICAL — see Loop 2 entry below.)*
- **HIGH-3: `audit: drop` regex collision risk.** Bare `audit:`
  prefix is generic; the required `\s*drop` constraint already
  excludes the typical collision (`audit: 0/55 actionable` at
  auditdialog.cpp:4242), but no negative-test INV locked the
  behavior. **Resolved:** § 2.6 expanded with a Collision-prevention
  paragraph; INV-12 added with three explicit negative cases.
- **HIGH-4: ADR-0003 vs spec naming inconsistency.** ADR-0003 dec
  4 listed `severityShiftForCorroboration` and `applyAllowlistDrops`
  as new pure functions; the spec uses `applyCorroborationShift`
  and never defines `applyAllowlistDrops` (the actual change is
  to widen the existing `allowlisted()` call site, not add a new
  function). **Resolved:** ADR-0003 dec 4 amended to match.
- **MEDIUM-1: `auditdialog.h:100-115` line range is loose.** The
  cited symbols span 107-115. **Resolved:** § 1.1 table tightened.
- **MEDIUM-2: off-by-default AI claim missing citation.**
  **Resolved:** § 9 now cites `Config::aiEnabled` at
  `config.cpp:734`.
- **MEDIUM-3: per-check 100 cap missing citation.**
  **Resolved:** § 5 now cites `kMaxFindingsPerCheck` at
  `auditdialog.h:308`.
- **MEDIUM-4: `semgrepRulePacks` arg shape ambiguous.**
  **Resolved:** § 2.3 doc comment now specifies two consecutive
  list entries per framework with an example.
- **MEDIUM-5: Two new INVs needed for "Allow" + "Fold" UI
  workflows.** § 10 step 7/8 deferred them as UI-tested but
  no audit trail covered the orchestration. **Resolved:**
  INV-13 (Allow file-write contract) + INV-14 (Fold orchestration
  helper-invocation contract) added.
- **LOW: Various.** Status-line "TBD" replaced with
  "after self-cold-eyes loop"; `insertBlock` "idempotent on re-run
  only by content" reworded to "non-idempotent by design"; ROADMAP
  perms 0600 → preserve existing 0644; flock claim in ADR-0003
  amended to "adopts configbackup.h pattern" (no existing
  mechanism); ADR-0003 cold-eyes review pass section reformatted
  to lead with "Findings raised:" before "Resolutions:".
- **DROPPED (unverified):** "Severity Blocker should be Critical"
  — `auditengine.h:48-49` confirms `Critical = 3, Blocker = 4` in
  the project enum, so Blocker IS the highest tier. The spec was
  correct.

### Loop 2 (2026-05-13)

Same reviewer briefed cold against the loop-1 edits. Findings:

- **CRITICAL: invented `## [Unreleased]` ROADMAP convention.**
  ROADMAP.md does not have `[Unreleased]` today; roadmap-format.md
  § 3.7 specifies `## X.Y.Z — theme — (target: YYYY-MM)` for active
  blocks. The loop-1 fix invented a CHANGELOG-mirror convention
  that other tooling (Roadmap dialog) doesn't expect.
  **Resolved:** § 2.5 algorithm rewritten — caller passes
  `releaseBlockHeading` explicitly (no auto-create); new helper
  `findActiveReleaseHeading()` computes a default; dialog modal
  shows it editable. INV-8 rewritten for found/not-found cases;
  INV-8b added for `findActiveReleaseHeading()`.
- **HIGH: INV-12 reasoning self-contradicted (`\b` claim).**
  The third negative case ("the audit: drop logic was rewritten")
  was flagged as overmatch. Tracing the actual code path
  (`auditdialog.cpp:2083` rule-list parser): the suffix tokens
  fail to match any real rule id, so `commentSuppresses` returns
  false. The case IS safe — but the loop-1 wording cited `\b`,
  which doesn't actually defend against this. **Resolved:**
  § 2.6 rewritten with the two-layer defence (verb constraint +
  rule-list parser, citing `auditdialog.cpp:2083-2107`); INV-12
  re-stated to lock the safe cases without invoking incorrect
  reasoning.
- **MEDIUM: INV-9 over-match concern (bare `audit: drop`).**
  The loop-2 reviewer suggested requiring `=rule` form. **Resolved
  as accepted-by-design:** the bare form (suppress everything)
  matches the existing `ants-audit: disable` semantic and the
  user-experience contract. The literal `// audit: drop` in
  non-directive prose is contrived enough that the parity with
  the existing token wins. The two-layer defence (loop-2 § 2.6
  rewrite) covers the realistic over-match cases.
- **MEDIUM: build-cost LoC undercount.** **Resolved:** § 6
  totals raised to 1450 LoC, caveat added that the count is
  hand-written code only (moc regen excluded).
- **MEDIUM: RuleQualityTracker accessor not specified.**
  **Resolved:** § 2.4 doc comment specifies the new accessor
  `noisyRuleIds(fpThreshold=50, minSamples=5) const` (~15 LoC,
  added to RuleQualityTracker, locked in § 6 build cost +
  § 7 files-touched).
- **LOW: Boundary paragraph dup of ADR-0003.** **Resolved:**
  spec frontmatter trimmed to a one-line cite.
- **LOW: "after self-cold-eyes loop" ambiguous.** **Resolved:**
  status line now reads "after 2-loop cold-eyes convergence".
- **LOW: Files-touched mixes implementation + ship phase.**
  **Resolved:** § 7 split into "Implementation phase" and
  "Ship phase" subsections.

### Loop 3 (2026-05-13)

Same reviewer briefed cold against loop-2 edits. Findings:

- **HIGH: example heading `0.7.88 — TBD (target: 2026-05)`
  used `TBD` placeholder which doesn't match the convention.**
  **Resolved:** all instances → `0.7.88 — audit fold (target:
  2026-05)`.
- **MEDIUM: INV-12 narrative said "literally-empty tail" but
  missed the `--`-prefixed branch at `auditdialog.cpp:2071`.**
  **Resolved:** INV-12 narrative now lists both bare-suppress
  tails (a) empty and (b) `--`-prefixed.
- **MEDIUM: § 2.5 doc-comment said `lstrip("\n")` of trailing
  newline — direction inverted.** **Resolved:** changed to
  `rstrip("\n")`.
- **LOW: Loop-2 audit trail's HIGH-2 entry described the
  loop-1 `[Unreleased]` fix as if still current, ignoring its
  loop-2 reversal.** **Resolved:** appended `(Superseded by
  loop-2 CRITICAL — see Loop 2 entry below.)` to the HIGH-2
  resolution.

### Convergence

Three loops:
- Loop 1: 4 HIGH + 5 MEDIUM + 5 LOW (1 dropped as unverified).
- Loop 2: 1 CRITICAL + 1 HIGH + 3 MEDIUM + 3 LOW.
- Loop 3: 1 HIGH (cosmetic) + 2 MEDIUM (mechanical) + 1 LOW
  (audit-trail polish).

Loop 3 surfaced no design-level findings — only mechanical
wording. Convergence pattern is healthy: severity drops each
loop, fixes get more local. Stopping at loop 3 is justified.

Final status: ship-ready after 3-loop convergence (2026-05-13).

### amend (2026-11-27)

Not a review loop; no reviewer was dispatched. ANTS-6980:
§ 2.1 and § 3.2 now describe the shipped heading text box (the whole
actionable set folds under a confirmed release heading); the
per-finding checkbox modal stays a v2 idea. Rule chosen 2026-11-27
(user delegated). § 12's line citations dropped.
