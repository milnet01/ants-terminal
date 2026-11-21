# ANTS-1876 — `workspace_search` payload knobs — review loop log

Record for [`../specs/ANTS-1876-workspace-search-payload-knobs.md`](../specs/ANTS-1876-workspace-search-payload-knobs.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

### ANTS-3548 amendment (default-ON `max_match_bytes`) — 2026-07-17

- **Loop 1 (2026-07-17).** C0 · H3 · M1 · L2 · I1. The default-flip
  amendment updated its five nominated touchpoints (Status, §2.1, §2.4,
  INV-1, INV-6) self-consistently but left three unreconciled
  contradictions elsewhere: §7 Migration still claimed "None / existing
  calls byte-identical" (HIGH); §8 + the schema still said "default 0 /
  disables" (HIGH); the mirror test spec INV-1 still said "falls back to
  0" and lacked the ANTS-3548 anchors (HIGH). Plus §2.4 present-tense
  provenance paragraph contradicting itself (MEDIUM), §2.4 "caller's
  input activated" imprecise under default-ON (LOW), §2.1 missing the
  non-numeric→512 case (LOW). All H/M/L verified + fixed: rewrote §7 to
  state the deliberate non-byte-identical default change, updated §8 to
  require schema `default 512`, past-tensed the §2.4 provenance para,
  added the type-mismatch note, updated the mirror test spec.
- **Loop 2 (2026-07-17).** C0 · H1 · M0 · L1 · I1. Cold read caught a
  contradiction the loop-1 fixes surfaced: §8 set `default 512` but left
  the schema `minimum: 50`, so the `0` opt-out (now the sole off-switch)
  would be below-range and rejected by a bounds-validating client
  (HIGH). Also the mirror test spec invented free-standing INV-20/INV-21
  numbers with no §3 counterpart (LOW), and its Pre-fix section didn't
  state the ANTS-3548 fail-first basis (INFO). Fixed: §8 now lowers the
  schema `minimum` to 0 (0 legal + passable; server still clamps 1..49
  up to 50); relabelled the test-spec rows "INV-1 (ANTS-3548)"; added
  the fail-first note.
- **Loop 3 (2026-07-17).** C0 · H0 · M0 · L1 · I1 — polish convergence.
  Schema guidance coherent across §2.1/§8/INV-1/INV-6/§7 + the mirror
  test spec; no residual old-value assertion as current behaviour. Fixed
  the two polish nits: a garbled `effective-value-> 0` in INV-6, and a
  §2.2 "minimum value (50)" that collided with the schema `minimum: 0`
  the amendment introduced (reworded to "clip-budget floor"). Converged.

### Original ANTS-1876 (2026-05-26)

- **Loop 2 (2026-05-26).** Reviewer flagged 0 CRITICAL, 2 HIGH
  (INV-2 ambiguous: clip "no more than" could be satisfied with
  ellipsis appended *outside* budget — should be exact ==
  budget; §2.5 conflated load-bearing dedup-key stability with
  mechanical context-entry clip), 3 MEDIUM (INV-7 source-grep
  end-anchor too vague; §2.4 echo silent on error-envelope
  behaviour; INV-5 silent on `also_at` clip-immunity), 4 LOW/INFO
  (cosmetic). All verified against `cmdWorkspaceSearch` paths.
  Fixes: INV-2 reworded to "exactly `max_match_bytes` bytes when
  clipped, verbatim when unclipped"; split §2.5 into load-bearing
  vs mechanical with explicit pipeline order; INV-7 anchor
  tightened to `props["max_match_bytes"]` / `props["headline_only"]`
  literals; INV-6 amended to "echo only on ok:true"; INV-5
  amended with `also_at` clip-immunity.
- **Loop 1 (2026-05-26).** Reviewer flagged 1 CRITICAL (arg name
  divergence: my `summary_only` ≠ ROADMAP's `headline_only`; also
  shape divergence `{file, line}` vs `{file, line, headline}`),
  3 HIGH (envelope-echo "existing pattern" claim factually wrong —
  existing code echoes unconditionally; ROADMAP options 3 & 4 not
  surfaced as deferred; context-array clip rule not locked by an
  INV), 3 MEDIUM (INV-1 "existing parse idiom" citation didn't
  match the named code paths; INV-5 silent on dedup vs summary
  ordering; LoC budget tight), 4 LOW/INFO (ellipsis-inside-budget
  ambiguity; fail-first sentence missing in §4; cross-doc impact
  missing CHANGELOG + descriptor touchpoints; etc.). All verified
  against `src/remotecontrol.cpp` (4339 `context` parse; 4354
  `timeout_sec` parse; 4587-4612 dedup; 4626-4630 unconditional
  envelope echo) and ROADMAP bullet 14331-14338. Fixes: rename arg
  to `headline_only` matching ROADMAP source-of-truth; emit
  `{file, line, headline}` triples per Vestige; rewrite INV-6 to
  "echo when feature activated" with explicit back-compat rationale
  vs the existing unconditional pattern; add §6 Out of scope
  naming ROADMAP options 3 + 4 as follow-ups; extend INV-2 / INV-3
  to cover context-array entries explicitly; pin INV-1 citation to
  `context`'s actual idiom; add §2.5 explicit dedup-vs-clip
  ordering; clarify ellipsis-inside-budget rule; add fail-first
  sentence in §4; bump CHANGELOG into §9 cross-doc impact; bump
  LoC budget; renumber sections to accommodate new §6.
