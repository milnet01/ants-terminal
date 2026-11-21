# ANTS-2023 — soft-warn a `cat`/`head`/`tail`-of-source dump into a non-blocking `file_outline`/`read_region` nudge — review loop log

Record for [`../specs/ANTS-2023-soft-warn-cat-head-tail.md`](../specs/ANTS-2023-soft-warn-cat-head-tail.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two independent cold reviewers per loop (accuracy lane + consistency lane),
briefed with no prior-loop findings. Verified fixes by severity:

- **Loop 1** — 2 HIGH (the `ANTS_NUDGE_TOOL` fallback was never named → would
  mis-bucket reads as `grep`; INV-5 disjointness loosely justified), 3 MEDIUM
  (`build-`/`build` exemption untested vs the live `build-fast/` tree; INV-6
  missing expected counter count; `cat ROADMAP.md` disjointness prose), 3 LOW
  ("first non-flag operand" undefined; EXEMPT-ordering note; §6 manual-smoke
  parity), 1 INFO→fixed (INV-3 asserted a guessed byte size). Scope trim: dropped
  interactive pagers `less`/`more` from the tool set.
- **Loop 2** — 2 HIGH (INV-5 pinned to the real mechanism; INV-2 reworded to
  "predicate returns 1, falls through"), 4 MEDIUM (INV-6 record-on-both-paths;
  blind-spot (d) separated option-args; INV-3 measured 225 B; ROADMAP block
  pattern keys on `grep`+`ROADMAP.md`, not a literal pipe), 4 LOW (`:101` cite;
  "13 items" + `test_hooks.sh:2-3`; `bat` non-TTY note; INV-4 EXEMPT-vs-non-eligible
  relabel), 1 INFO (token estimate + operand-walk-is-new-logic note).
- **Loop 3** — 0 HIGH (the "225 B is wrong" HIGH **dismissed as unverified** —
  file-extracted `wc -c` = 225 B), 1 MEDIUM (INV-5 reconciled with EXEMPT-first
  ordering), 2 LOW (blind-spot (e) leading-token wrappers; append-after-2141-bullets).
- **Loop 4** — 0 HIGH (byte-count dismissed again), 1 MEDIUM (INV-6 `:43`/`:46`
  cite order), 3 LOW (INV-3 shared-cap-not-count; `bat` claim softened; redirect
  scan documented `$cmd`-wide).
- **Loop 5** — 0 CRITICAL/HIGH (both reviewers withdrew their HIGH on re-check and
  called the spec "unusually clean"), 2 MEDIUM-rated cosmetic clarity fixes
  (disambiguate the `:43`/`:46` cite as the grep-branch shape the read branch
  mirrors; note `git grep`'s two-token dispatch does not weaken the INV-5 proof).

Byte count `225 B` re-flagged on loops 3/4 by reviewer intuition each time and
dismissed each time against the file-extracted measurement; loop 5 did not
re-flag it after the em-dash math was added to INV-3.
