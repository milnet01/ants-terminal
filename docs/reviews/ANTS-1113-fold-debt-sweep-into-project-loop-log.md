# ANTS-1113 — Fold `/debt-sweep` into the Project Audit tool — review loop log

Record for [`../specs/ANTS-1113-fold-debt-sweep-into-project.md`](../specs/ANTS-1113-fold-debt-sweep-into-project.md) § 11. Cold-eyes log.

## Cold-eyes loop log

### Loop 1 — 2026-05-13

Two-lane partition (API surface + invariants/cross-doc). Findings
verified and fixed:

- **CRITICAL (1).** § 3.1 step 3-6: `FeatureCoverage::buildProjectSourceBlob`
  + `existsInSource` were referenced as if they existed. Both are
  in fact file-local in `featurecoverage.cpp`. Resolution: spec
  now commits to extracting them as new public helpers; § 6 LoC
  budget grown +70 LoC (from +40 to +110).
- **HIGH (5).**
  - § 3.1 step 5: `extractSpecTokens` is backtick-fenced only;
    can't be reused for bare comment tokens. Fixed: spec a new
    `\b([A-Z][A-Za-z0-9_]{3,})\b` regex restricted to leading-cap
    CamelCase (false-positive control).
  - § 3.6 step 2: `extractTopVersionBullets` is top-section-only.
    Fixed: pin the `[Unreleased]` reuse to Keep-a-Changelog
    convention with a verifiable check.
  - § 2.2: `resolveRootCanonical` is at line 1040, not 1041.
    Fixed.
  - § 10: cited a "Ants Terminal-managed projects use the in-app
    fold" sentence that doesn't exist in `~/.claude/skills/debt-sweep/SKILL.md`.
    Fixed: drop the citation, re-word as "skill ships unchanged;
    orchestrators detect via tools/list".
  - INV-13: `applyMechanicalFix` returned bool, couldn't
    disambiguate "file changed" from "IO error". Fixed: engine
    return widened to `ApplyVerdict {bool applied, QString
    errorCode, QString errorMessage}`; INV-13 + § 2.2 envelope
    updated to assert specific `error_code` strings.
- **MEDIUM (6).** templateDebtSweepFoldInBlock literal heading +
  bullet template now pinned in § 3.10. `debt_sweep_defer`
  envelope clarifies success-vs-no-block-vs-failure. ANTS-1259
  reservation re-worded (atomic claim retracted; describes
  hand-edit convention used by ANTS-1257/1258). INV-N regex
  extended to `\bINV-([0-9][0-9a-zA-Z]*)\b` to cover `INV-8b`
  shape used in ANTS-1111. "Per-release vs per-keystroke"
  rationale corrected (debt-sweep can be invoked mid-dev). § 2.1
  doc-comment for `detectStaleTypeComments` updated to reference
  the canonical extension list.
- **LOW (3).** § 12 explicitly noted as super-set of § 1.1 (not
  dedup). § 1.1 / § 2.2 self-reference to ANTS-1112 dropped (it's
  itself a reference). § 8 added a row for unsupported
  `detectOrphanQUnused` shapes (C++17 structured bindings, brace
  init, templated types) so v2 has a tracked extension list.

### Loop 2 — 2026-05-13

Single-lane verification pass (no NEW lane partition needed —
spec is one cohesive doc). Findings verified and fixed:

- **HIGH (2).**
  - § 3.1 step 3 only committed to extracting blob builder +
    existsInSource, but § 6 also budgeted exposing
    `kSpecStopwords`. Fixed: § 3.1 step 3 now enumerates three
    new public surfaces (a) blob builder, (b) existsInSource,
    (c) `specStopwords()` accessor — single source of truth
    matching § 6 LoC budget.
  - ANTS-1112 § 1.1 (line 57, post-shipped) used "atomic from
    user's POV" wording for the v2-ID reservation; ANTS-1113
    loop-1 retracted "atomic" but didn't acknowledge the sibling
    drift. Fixed: § 1.1 ANTS-1259 ID note now adds a
    "Note on ANTS-1112's atomic claim" supersession paragraph;
    ANTS-1112 left as-shipped (post-release spec edits have
    lower value than catching the next instance correctly).
- **MEDIUM (4).**
  - § 2.2 was ambiguous on cmdDebtSweep* vs registerToolProvider
    pattern (both? alternative?). Fixed: § 2.2 now explicit
    about ANTS-1112 dual-surface convention — RemoteControl
    method = implementation layer, registerToolProvider lambda =
    registration layer, both are needed.
  - § 3.10 didn't pin `<dateIso>` format. Fixed: pinned to
    `YYYY-MM-DD` (date-only ISO 8601, no time component, no `T`
    separator) matching `roadmap-format.md` § 3.8 example.
  - `debt_sweep_defer` envelope didn't address the ID-leak risk
    when `written:false` (allocateIds bumps eagerly; no
    rollback). Fixed: envelope row now documents the ID-leak
    trade-off explicitly + matches ANTS-1111 § 2.5's
    same-trade-off documentation.
  - INV-10 didn't assert `<dateIso>` byte-equality between
    heading and Source: lines. Fixed: invariant now extracts
    both via regex and asserts string equality.
- **LOW.** § 11 loop-1 wording "extended" the INV regex was
  technically a narrowing (numeric-leader required). Cosmetic;
  left as-is.

### Loop 3 — 2026-05-13

Convergence-check pass against the same lane partition. Zero
CRITICAL/HIGH/MEDIUM/LOW verified findings; one INFO that the
spec already self-flags inline (the `.roadmap-counter` snapshot
at line 65 will silently age if anything else allocates ID 1259
before this ship — already documented in the same paragraph).
Spec is signed off for implementation.

### Loop 4 — 2026-07-13 (post-ship maintenance sync)

Triggered by the ANTS-3342 edit to § 3.5 (window-scoping the
`shipped_without_commit` detector). One cold reviewer, one pass.
Verified findings: HIGH ×3, MEDIUM ×2, INFO ×1 — all doc-vs-code
staleness, all fixed. HIGH: the `ScanOptions` scope comment still
listed the ROADMAP cross-check as ignoring `sinceRef` (§ 2.1); the
detector doc-comment still described the whole-ROADMAP algorithm
(§ 2.1); `templateDebtSweepFoldInBlock`'s signature was missing the
`idPrefix` parameter added by ANTS-3497 (§ 2.1 / § 3.10) — a
pre-existing drift surfaced in passing, not introduced by ANTS-3342.
MEDIUM: INV-5 lacked the in-window-flip precondition (§ 4); the § 5
perf row omitted the added `git diff` and mis-sized `I`. INFO: this
Status header still read "3-loop convergence (2026-05-13)".

The loop-2 cold re-read confirmed every ANTS-3342 fix held (§ 3.5 ↔
code ↔ INV-5 ↔ test all matched), so the window-scoping edit is
converged. That pass also surfaced a *separate* class of pre-existing
drift unrelated to ANTS-3342 — the spec has fallen behind the ANTS-1358
detector expansion (4 detectors + `staleTodoMaxAgeDays` unlisted) and
the ANTS-2227 dry-run `ApplyVerdict`/`applyMechanicalFix` surface.
That reconciliation is tracked as ANTS-3510 (out of this edit's lane),
not fixed here.
