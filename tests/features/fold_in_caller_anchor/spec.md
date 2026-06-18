# fold_in_caller_anchor — cold_eyes/indie_review fold-in anchor to caller_cwd (ANTS-1630)

`cold_eyes_fold_in` and `indie_review_fold_in` used to gate their ROADMAP
write on the **focused tab** (`RcGate::checkCallerCwd(resolveRootCanonical(
m_main), …)` then `root = gate.focused`), refusing `cwd_mismatch` whenever an
orchestrating session's `caller_cwd` differed from the focused tab. ANTS-1630
anchors both writes to the caller's own `caller_cwd` via the canonical
`ants::resolveCallerCwdRoot` decoder (ANTS-1401), so a `/cold-eyes` or
`/indie-review` run folds into its own project regardless of which tab is
focused. Full design + cold-eyes loop log: `docs/specs/ANTS-1630.md`.

The handlers are GUI-bound (live `MainWindow` + `RemoteControl`), so the
contract is pinned by source-scrape of each handler's definition body window —
the same technique `cold_eyes_fold_in_narrative` uses.

## Invariants

- **INV-1 / INV-2** — each handler derives `root` from
  `ants::resolveCallerCwdRoot(` and does **not** read `gate.focused`.
- **INV-2 / INV-5** — neither handler still calls `RcGate::checkCallerCwd(`
  (they migrated off the focused-tab gate; the resolution mirrors
  `test_audit_fold_in`'s anchor-at-dir shape).
- **INV-3** — each handler refuses `cwd_bad` (the gate's own pre-existing
  code, preserved — no contract change) when `caller_cwd` does not resolve to
  a directory, gated on `rr.cwd` empty or `!isDir()`.

## Out of scope

The dispatcher's `caller_cwd_required` refusal for an *absent* `caller_cwd`
(unchanged — both verbs are `CallerCwdContract::Required`); the other six
verbs still using `RcGate::checkCallerCwd` (their S1 call-site count is
covered by `roadmap_fold_in`'s S1 test, lowered 7→6 in the same change).
