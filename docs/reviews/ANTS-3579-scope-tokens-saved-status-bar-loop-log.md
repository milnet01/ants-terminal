# ANTS-3579 — Scope the tokens-saved status-bar pill to the current project — review loop log

Record for [`../specs/ANTS-3579-scope-tokens-saved-status-bar.md`](../specs/ANTS-3579-scope-tokens-saved-status-bar.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 0** (draft, 2026-07-20): drafted against ANTS-3572 + verified enablers
  (`ants-explore` subsystem report). Orchestrator mechanical pre-pass fixed 3
  cites before dispatch (`Tracker::baselineFor` qualification;
  `claudeintegration.h:607`; the `onTabChanged` refresh-path clarification).
- **Loop 1 — two independent cold reviewers (2026-07-20):** tally **CRITICAL 1 ·
  HIGH 4 · MEDIUM 5 · LOW 4 · INFO 2** (all verified against source). Fixes:
  - **CRITICAL (C1) / MEDIUM (M2)** — INV-1's per-call formula (`baseline −
    outBytes`, per-call floor) diverged from the engine
    (`max(0, baseline·nCalls − (bytesIn+bytesOut))`, `tokenusageengine.cpp:131-134`)
    and INV-8 wrongly claimed `Σ(per-project) == global ± #tools`. Fixed: formula
    now subtracts `argBytes + outBytes`; INV-8 reframed as a deliberate per-call
    estimate that is NOT equal to the aggregate — the "All projects" line reads
    the authoritative global (INV-4), never the per-project sum; the rounding test
    moved to the display byte→token path.
  - **HIGH (H1)** — `recordDispatch` cannot call `resolveCallerCwdRoot(mainWindow(),
    …)`: `ClaudeIntegration` has no `MainWindow` (verified `claudeintegration.h`).
    Fixed: attribution key derives from `caller_cwd` via the `static`
    `QFileInfo::canonicalFilePath()` (no `MainWindow`); new INV-11 pins the single
    derivation; § Builds-on de-scoped `resolveCallerCwdRoot`.
  - **HIGH (H2)** — attribution key vs. focused-display key can diverge on a
    subdir / `cd`. Fixed: § 2.2 makes both use the same `canonicalFilePath()` and
    documents the accepted divergence (stable per-session `caller_cwd`, no
    marker-walk) + the deleted-dir case in § 4.
  - **HIGH (H3)** — the `MainWindow` fold could not iterate the private live map.
    Fixed: added the `sessionSavedBytesByProject()` snapshot getter (§ 2.3).
  - **HIGH (H4)** — per-call `canonicalFilePath` `stat` was unbudgeted and broke
    ANTS-3572 § 6's "no disk I/O". Fixed: new § 6 budgets it + the single-entry
    memo mitigation (§ 2.1).
  - **MEDIUM (M1)** — INV-7 tested a Qt signal-connect that does not exist
    (`onTabChanged` is a method). Fixed: INV-7/§ 2.2 retarget the render into the
    existing `onTabChanged` body.
  - **MEDIUM (M5/M6)** — persisted eviction was date-granular (tie-ambiguous) and
    used a different policy from the live map. Fixed: `updated` is an ISO
    datetime + root-string tie-break (INV-5b); live map now evicts
    least-recently-touched, matching "oldest wins".
  - **MEDIUM (M7)** — no failure-modes section. Fixed: added § 4.
  - **LOW** — `mainwindow.cpp:5768` is `endTokenSession()` (fold via signal), not
    a direct fold call (§ Builds-on); tooltip restores the per-project `(since …)`
    date; `humanize` → `humanizeCount`; section gap §6→§9 closed (now §4 Failure
    modes / §5 Tests / §6 Perf / §7 Migration / §8 Non-goals / §9 log).
  - Loop 2 ran cold (no fix briefing).
- **Loop 2 — two independent cold reviewers, cold (2026-07-20):** none of Loop 1's
  findings resurfaced (the fixes held). Tally **CRITICAL 0 · HIGH 1 · MEDIUM 5 ·
  LOW 4 · INFO 2** (all verified). Fixes:
  - **HIGH (H-a)** — no invariant pinned the live-map clear to run AFTER the
    synchronous fold; a clear-before-emit would fold an empty map (silent
    per-project loss). Added INV-12 + the modified `endTokenSession()` body (clear
    after `reset()`) — the per-project analogue of ANTS-3572 INV-3.
  - **MEDIUM (M-A)** — INV-8's `/kCharsPerToken` display scrape was unsatisfiable
    (constant is file-local in `tokenusageengine.cpp`'s anon namespace; the
    ANTS-3572 chip uses a bare `/ 4`). Fixed: promote `kCharsPerToken` to a public
    `constexpr` in `tokenusageengine.h` (§ 2.5) so the display path + test key on
    the symbol.
  - **MEDIUM (M-B)** — INV-1's "pure test of the byte math" referenced no pure
    function (math is inline in `recordDispatch`) → tautological. Dropped the pure
    test; INV-1 is pinned by the exact-expression source-scrape.
  - **MEDIUM (M-a)** — the single-entry `caller_cwd` memo could not cover
    interleaved multi-project calls (§ 1's core case). Fixed: bounded memo map
    (≤ `kMaxTokenProjects`, cleared on overflow / at reset).
  - **MEDIUM (M-b)** — ANTS-3572 § 2.1's "signal-only, not onTabChanged" was left
    stale. Added a supersession forward-reference to ANTS-3572 § 2.1 + a note here.
  - **MEDIUM (M-c)** — the "All projects" global-session term's getter was unnamed
    after the payload demotion. Named it: `m_integration->tokenUsageReport(false)
    .totalSaved` (§ 2.4).
  - **LOW** — INV-10 retitled ("Toggle unchanged; a11y names the project scope");
    intra-tab `cd`/split-pane staleness noted accepted (L-a); live-map eviction
    discards accumulated bytes, not just recency (L-b); INV-3 tag reconciled with
    § 2.4 (L-c); `kMaxTokenProjects` home stated (I-A); single-project two-totals
    artifact noted (I-a).
  - Loop 3 ran cold (no fix briefing).
- **Loop 3 — two independent cold reviewers, cold (2026-07-20):** no Loop 1/2
  finding resurfaced. Tally **CRITICAL 0 · HIGH 1 · MEDIUM 1 · LOW 5 · INFO 1**
  (all verified) — all mechanical/propagation, no new design defect. Both lanes
  independently flagged the same top item. Fixes:
  - **HIGH/MEDIUM (M1)** — the Loop-2 M-a memo fix (single-entry → bounded map)
    was not fully propagated: § 6's mitigation and § 2.1's intro still named the
    phantom `m_lastCallerCwd → m_lastCallerRoot` single slot, contradicting
    § 2.1's `m_callerCwdRootMemo`. Fixed both to the bounded map.
  - **MEDIUM (M2)** — the per-project tooltip month/YTD named no extraction; an
    implementer would hand-roll a year sum. Fixed: § 2.4 names the reuse of
    `TokenUsageEngine::sumYear` + a `monthly[currentMonthKey]` lookup (the same
    helpers the global map uses).
  - **LOW/INFO** — eviction now drops the victim from `m_projectTouchSeq` too
    (L-2); § 6 RAM tally counts all three live maps (L-1) + the > 64-caller_cwd
    memo-cap note (I1); `foldProjectBucket` `since = nowIso.left(10)` and the fold
    reuses the global `monthKey`/`nowIso` (L-3); INV-10's a11y test keys on the
    focused-project variable at `:530` (L1); § Distinct-from cite split into
    `cmdTokenUsage` (~:19352) + `tokenSavingsSummary()` (~:19357) (L2); the face
    divides bytes→tokens once, no double-count (Lane-A open q, § 2.4).
  - Loop 4 ran cold (no fix briefing).
- **Loop 4 — two independent cold reviewers, cold (2026-07-20):** no Loop 1/2/3
  finding resurfaced. Tally **CRITICAL 0 · HIGH 2 · MEDIUM 3 · LOW 4** (all
  verified). Two genuinely NEW substantive items (this is why the loop kept
  running):
  - **HIGH (H-1, visibility)** — the pill hid whenever the focused project's
    *live-session* saved was ≤ 0, so a tab switch to a project with only stored
    history hid it (and its tooltip) — defeating the feature. Fixed: visibility
    gates on `(stored lifetime + live session) > 0`; the face shows the session,
    falling back to all-time when the session is 0. **Flagged as a UX decision for
    sign-off** (§ 2.4 / INV-2).
  - **HIGH (H1, wiring)** — `onTabChanged` is `MainWindow::onTabChanged` (`:2966`)
    → `refreshStatusBarForActiveTab()` (`:5844`) → the controller's *named* refresh
    methods; the tokens-saved render is a lambda (`claudestatuswidgets.cpp:488`),
    and the controller has no `onTabChanged`. Fixed: extract a named
    `refreshTokensSavedChip()` called by both the signal slot and
    `refreshStatusBarForActiveTab()`; INV-7 test re-filed across `mainwindow.cpp` +
    `claudestatuswidgets.{h,cpp}`.
  - **MEDIUM (M-2)** — per-root `foldProjectBucket` eviction was fold-order
    dependent (could drop a not-yet-folded root and re-seed it at lifetime 0 in the
    > 64-root zone). Fixed: `foldProjectBucket` no longer evicts; a single
    `pruneProjectBuckets` pass evicts over the union after the loop (INV-5b).
  - **MEDIUM (M-1)** — "division exactly once at display" contradicted the
    fold-time division. Fixed: "once per data-path" (display for live, fold for
    stored; never twice on one value) in § 2.1 / INV-8.
  - **MEDIUM (Lane-A M1)** — INV-1's exact-`max(0,…)` scrape would miss the engine's
    `std::max<qint64>` idiom. Fixed: key on tolerant symbols.
  - **LOW** — fold loop threads the accumulator return (L-1); the face divides
    bytes→tokens once and the ≤ 0 guard is on tokens (L-2); reuse the existing
    `succeeded` local (Lane-A L1).
  - Loop 5 ran cold (no fix briefing).
- **Loop 5 — two independent cold reviewers, cold (2026-07-20): DESIGN CONVERGED.**
  No Loop 1/2/3/4 finding resurfaced. Tally **CRITICAL 0 · HIGH 0 · MEDIUM 3 ·
  LOW 5** — every finding a **mechanical doc-vs-code precision** item; no new
  design / structural / architectural defect in either lane (both pronounced the
  design implementable). Fixes:
  - **MEDIUM (Lane-B M2 / Lane-A accuracy)** — the controller reaches the focused
    tab via `m_focusedTerminalProvider` (`std::function`, `claudestatuswidgets.h:515`),
    not a `focusedTerminal()` method. Corrected § 2.2 / § 2.4 / Builds-on.
  - **MEDIUM (Lane-A M1)** — the "mirror `refreshModelStateChip`" exemplar was
    wrong: that method is status-**timer** driven (`m_statusTimer::timeout`,
    `mainwindow.cpp:800`), not tab-driven. Re-anchored to the methods actually
    invoked by `refreshStatusBarForActiveTab` (`refreshRoadmapButton` /
    `refreshRepoVisibility`, `:5942-5943`); the INV-7 test was already correct.
  - **MEDIUM (Lane-B M1)** — the `endTokenSession` code comment "chip → hidden"
    contradicted the new stored-or-session visibility (post-fold the focused
    project's all-time > 0 → visible). Comment corrected.
  - **LOW** — visibility guard terms pinned to tokens (L1); "the two caps agree"
    softened to "least-recently-active spirit" (L2); `foldProjectBucket` call
    names `keepMonths=24` (L3); duplicate `L1` tag renamed `LA1`; INV-2's vacuous
    negative grep replaced with a falsifiable positive check.
  - **Convergence:** the DESIGN is stable and clean across 5 independent cold
    passes (last CRITICAL was Loop 1; last HIGH was Loop 4, fixed). Loop 5
    surfaced only doc-vs-code precision, all applied. Per the cold-eyes cap
    (`--max-loops 5`): the mechanical items are fixed and the exact wiring names
    (`refreshTokensSavedChip`, `m_focusedTerminalProvider`, `pruneProjectBuckets`)
    are pinned by the § 5 test-first scrapes at implementation time. Spec surfaced
    for sign-off; the one open **UX decision is H-1** (tab-switch visibility on a
    history-only project — § 2.4 / INV-2 default: show it).
