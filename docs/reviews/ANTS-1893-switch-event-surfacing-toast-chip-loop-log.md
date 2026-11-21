# ANTS-1893 — Switch-event surfacing: toast + chip-pulse + Undo — review loop log

Record for [`../specs/ANTS-1893-switch-event-surfacing-toast-chip.md`](../specs/ANTS-1893-switch-event-surfacing-toast-chip.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 — 2026-05-28
- **CRIT C1** (themes/*.qss doesn't exist): rerouted §2.3, §2.6,
  §6, §8 to `src/themedstylesheet.cpp:13` `buildAppStylesheet`
  with `%5` accent placeholder.
- **HIGH H1** (fabricated theme-file verification claim): dropped.
- **HIGH H2** (mainwindow.cpp:728 cite wrong line): fixed to L725
  + L728 disambiguated.
- **HIGH H3** (`onUndoSwitchClicked` use-after-clear bug):
  captured `priorTier` before clearing pending fields.
- **MED M1-M6**: insertion-order clarified;
  rec.reason shapes documented; toast-format consistency
  (parens); CLAUDE.md anchor by bullet; `emitSwitchSurfacing`
  public test seam justified; status-bar.md not dialogs.md.

### Loop 2 — 2026-05-28
- **CRIT** (use-after-clear): held (no resurfacing).
- **HIGH H1-H3**: fabricated theme-verification + line-cite drift
  + use-after-clear — all held.
- **HIGH** (new): chip-padding math wrong; pulse-rule hover overlay;
  `tierFromModelId(priorTier)` misuse — folded.
- **MED M1-M5**: TR precedent, scoring-line cite, cross-spec line
  drift, helper visibility justified, double-Pairs-with — folded.

### Loop 3 — 2026-05-28
- **HIGH** (new): INV-7 per-tab Undo PID guard added; status-bar.md
  redirect verified.
- **MED**: setFlat acknowledge; sub-toggle enabled-mirror INV-14;
  default-true rationale.

### Loop 4 — 2026-05-28
- **HIGH H1** (border claim wrong, base has 1px): fixed — pulse rule
  changes only colour; padding stays 6px 14px (no text-shift).
- **HIGH H2** (ROADMAP toast vs status-bar Undo placement): §8(a)
  correction list expanded with 4 coordinated bullets.
- **HIGH H3** (mechanical predicate conjunction): citation fixed
  to L243-244.
- **MED**: pulse hover overlay rationale; pid_t include note;
  shortenReason reachability proof.

### Loop 5 — 2026-05-28
- **HIGH** (new): hover-overlay killing accent fill — clarified
  as INTENTIONAL ("pulse is dominant signal").
- **MED**: dead-PID branch, `<csignal>`/`<cerrno>` includes,
  toast-mute scope (INV-4), sub-toggle mid-window flip (INV-15).

### Loop 6 — 2026-05-28
- **HIGH** (new): Config mirror pattern (corrected to bool-pair
  precedent `claudeAutoModelNudgeShown`); toast-mute scope; INV-15
  toggle-flip-during-pending behaviour.

### Loop 7 — 2026-05-28
- **HIGH** (new): `::kill` includes (`<csignal>`/`<cerrno>`); pulse
  hover-overlay intent clarified; INV-15 enum-reset sentinel
  semantics documented as don't-care-when-string-empty.
- **MED**: Kind/Source taxonomy resolution; toast-failure scope.
- User gated stop at loop 7. Remaining LOW/INFO carry forward
  as implementation-time clarifications.
