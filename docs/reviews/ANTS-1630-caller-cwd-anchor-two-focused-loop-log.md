# ANTS-1630 — Caller-cwd-anchor the two focused-gated fold-in verbs — review loop log

Record for [`../specs/ANTS-1630-caller-cwd-anchor-two-focused.md`](../specs/ANTS-1630-caller-cwd-anchor-two-focused.md) § 6. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-17), 2 independent reviewers — folded:**
  - *Critical (both):* the original draft claimed all four verbs used
    `checkCallerCwd` + `gate.focused`; verified false — `roadmap_log`
    (`remotecontrol.cpp:4685`) and `test_audit_fold_in`
    (`testauditengine.cpp:1722`) are already caller-anchored. **Fix:** scope
    narrowed to the two genuinely focused-gated verbs; the two siblings are
    reframed as the existing precedent.
  - *High:* the draft sold a discarded "open-tab match" as a "far stronger
    identity proof." **Fix:** open-tab approach dropped; §1.2 now states the
    gate guarded against *accidental* mis-routing, not a hostile caller.
  - *High:* the draft's new `cwd_no_open_tab` code was unregistered. **Fix:**
    no new code; `mcp-error-codes.md` untouched.
  - *High/Medium:* S1 call-site arithmetic. **Fix:** verified 8 sites; 2
    migrate, 8→6; INV-6/T5 state the threshold change `7→6`.
  - *Medium:* header off-standard; dead spec cross-refs to ANTS-1621/1520.
    **Fix:** header to standard; cross-refs trimmed.
  - *Medium:* the "no session_id" claim over-reached. **Fix:** §1.2 scoped to
    the request body.
- **Loop 2 (2026-06-18), 2 reviewers — folded:**
  - *High (both):* §2 dropped the `!isDir()` arm and cited two siblings that
    refuse differently. **Fix:** the resolution mechanic was pinned to anchor
    *at* the caller dir with a full `isEmpty() || !isDir()` guard;
    `roadmap_log`'s downward walk demoted to "precedent, not template."
  - *Medium:* `bad_path` taxonomy mismatch. **Fix (superseded in loop 3):** the
    draft switched to `bad_cwd`; loop 3 reverted this to the gate's own
    `cwd_bad` (no contract change) — see below.
  - *Critical/Medium:* §1.2 said `SO_PEERCRED` "exposes `cred.pid`." **Fix:**
    reworded — Ants reads only `cred.uid`.
  - *Medium:* `Composes with` read like spec cross-refs. **Fix:** annotated
    `(ROADMAP)`. *Low:* `cmdRoadmapLogAppend` cite + missing `Layman:`. **Fix.**
- **Loop 3 (2026-06-18), 2 reviewers — folded:**
  - *High (B):* deleting the gate orphaned its absent-`caller_cwd` refusal.
    **Fix:** verified both verbs are `CallerCwdContract::Required`
    (`claudeintegration.cpp:9358/9370`), so the **dispatcher** already refuses
    absent `caller_cwd` with `caller_cwd_required` before the handler — that
    path is unchanged (§2 step 1, INV-3).
  - *High (B):* the loop-2 switch to `bad_cwd` *changed* the unresolvable-cwd
    code these verbs emit today (gate emits `cwd_bad`). **Fix:** reverted to
    `cwd_bad` — the fix now changes no refusal code at all. The `bad_cwd` vs
    `cwd_bad` taxonomy redundancy is surfaced as Open question 2.
  - *Medium (A):* the loop-2 open-coded canonicalisation conflicted with
    ANTS-1401 INV-1 ("only place that decodes `caller_cwd`"). **Fix:** §2 now
    reuses `resolveCallerCwdRoot` instead of a third hand-roll.
  - *Medium (B):* "sole consumer / no dialog refresh" was verified only for the
    handler bodies. **Fix:** §2 narrows the claim to the handler bodies and
    moves the downstream-signal check to Open question 4.
  - *Medium (B):* INV-4 "focused root" was loose (it's the focused-tab *shell
    cwd*). **Fix:** INV-4 reworded with the `caller==focused` equality proof.
  - *Medium (B):* test bundle membership under-specified. **Fix:** §4 names the
    sibling narrative-test bundle + the `SOURCES`-not-`add_executable` rule.
  - *Low (A/B):* Status line, S1 bare-string robustness note, `SO_PEERCRED`
    cite harmonised to `:1486-1490`. **Fix.**
- **Loop 4 (2026-06-18), 1 cold reviewer — clean pass.** Zero
  CRITICAL/HIGH/MEDIUM. Every load-bearing citation verified exact against live
  source (the `resolveCallerCwdRoot` accept-both crux, `EmptyFallback`
  unreachability, dispatcher `caller_cwd_required` attribution, 8→6
  arithmetic). Two LOW wording nits folded: INV-6's robustness rationale
  softened to present-tense; the dispatcher cite extended to `~8559-8585`.
  Spec accepted for implementation.
