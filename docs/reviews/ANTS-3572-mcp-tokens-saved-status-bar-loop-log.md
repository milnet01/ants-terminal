# ANTS-3572 — MCP tokens-saved status-bar chip + persisted month / YTD / all-time aggregate — review loop log

Record for [`../specs/ANTS-3572-mcp-tokens-saved-status-bar.md`](../specs/ANTS-3572-mcp-tokens-saved-status-bar.md) § 10. Cold-eyes review log.

## Cold-eyes loop log

- **Loop 0** (initial draft, 2026-07-18): drafted against the ANTS-1284 spec
  template + the `ants-explore` / `ants-status-bar` subsystem reports.
- **Loop 1 — architecture pre-verification (2026-07-18, orchestrator, before
  reviewer dispatch):** grep/Read-verified every cited anchor against current
  source. **1 CRITICAL fixed pre-dispatch:** the draft placed the fold in
  `ClaudeIntegration` via `Config::instance()` — but there is no such singleton
  (`Config` is `MainWindow`'s by-value `m_config`, `mainwindow.h:246`) and
  `ClaudeIntegration` holds no Config handle (`claudeintegration.h:78`); a second
  `Config` instance would clobber settings (`config.cpp:137` whole-object save).
  Rewrote § 2 to home the fold on `MainWindow` (the only layer with both
  `m_config` and `m_claudeIntegration`, built `mainwindow.cpp:3902`), driven by a
  new synchronous `tokenSessionEnding()` signal (INV-3). Confirmed anchors:
  `recordDispatch` def `claudeintegration.cpp:1439` (call sites 10562/10581),
  `contextUpdated` signal `claudeintegration.h:467`, initialize reset `:1575`,
  `resetTokenUsage()` `:337` (verb caller `remotecontrol.cpp:18981`), context-bar
  chip `claudestatuswidgets.cpp:118-128`, `cachedConfig()` `:65-81`/`:1304`,
  `.toBool(true)` idiom `config.cpp:508-541`, `RemoteControl::m_main`
  `remotecontrol.h:904`.

- **Loop 1 — two independent cold reviewers (2026-07-18):** tally **CRITICAL 1 ·
  HIGH 1 · MEDIUM 3 · LOW 5 · INFO 2** (all verified; the CRITICAL and HIGH are the
  same finding rated differently). Both reviewers independently re-confirmed every
  cited line anchor. Fixes applied this loop:
  - **CRITICAL/HIGH** — INV-10/§2.5/TSC-5 conflated the `token_usage` *inputSchema*
    (`additionalProperties:false` over `reset`/`include_zero`,
    `claudeintegration.cpp:7552`) with the *response* envelope. Verified: the verb
    has no response schema; the four fields go into the free-form `env` in
    `cmdTokenUsage`. Reworded INV-10/§2.5/TSC-5; the fields must NOT touch the
    inputSchema.
  - **MEDIUM** — (a) existing Config setters call `save()` internally
    (`config.cpp:511-513`), so "one write" was false → declared the three new data
    setters store-only (§ 2.3). (b) TSC-6 mislabeled INV-3 → relabeled INV-4/§2.4.
    (c) verb "four fields from `tokenSavingsSummary()`" but the POD has no `monthly`
    → clarified `monthly[]` reads the config map directly (§ 2.5).
  - **MEDIUM** — humanizer was a widget-TU static the engine test bundle can't link
    → moved to `TokenUsageEngine::humanizeCount` (pure, unit-testable; HUMAN-1 now a
    real test).
  - **LOW** — `total_saved` is tokens (÷4), not raw bytes (§ 1); `cachedConfig()` is
    a TU-local free function, not a controller member (§ 2.1); tooltip "since" date
    corrected to a first-fold date; `totalFailedBytes/4` annotated as `kCharsPerToken`.
  - **INFO** — added TSC-7 (INV-12 source-grep: no `totalFailedBytes` in the fold).
  Loop 2 to run cold (no fix briefing).

- **Loop 2 — two independent cold reviewers, cold (2026-07-18):** none of Loop 1's
  findings resurfaced (the fixes held). Tally **CRITICAL 0 · HIGH 0 · MEDIUM 5 ·
  LOW 6 · INFO 3** (all verified; both reviewers again re-confirmed every anchor).
  Fixes applied this loop:
  - **MEDIUM (substantive)** — the quit-path fold called `foldTokenSavingsIntoConfig()`
    **directly with no following reset**, and named two hooks (`closeEvent` /
    `aboutToQuit`); on a normal exit both could fire and fold the final session
    **twice** into month/YTD/all-time. Verified: `MainWindow::closeEvent` exists
    (`mainwindow.cpp:5735`), no `aboutToQuit` handler. Fixed by unifying all resets
    through `ClaudeIntegration::endTokenSession()` (emit→reset→refresh) — the sole
    fold trigger; the quit path calls it from `closeEvent` only, and its internal
    reset makes a repeat call a 0-total no-op (idempotent). §§2.2/2.4, INV-2.
  - **MEDIUM (substantive)** — `monthly[]` was routed through the **private**
    `m_config` (unreachable from `RemoteControl` → compile error). Moved `monthly`
    into the public `tokenSavingsSummary()` POD (which also trimmed 3 dead POD
    fields flagged separately). §2.5.
  - **MEDIUM (substantive)** — INV-3/TSC-2 guarded emit *ordering* but not the
    *connection type*; a `Qt::QueuedConnection` would fold a cleared counter.
    INV-3 now forbids it and TSC-2 greps for its absence.
  - **MEDIUM** — INV-2's "its only fold trigger" wording contradicted the (old)
    direct quit fold; reconciled by the `endTokenSession()` unification.
  - **LOW** — `additionalProperties:false` anchor `:7553`→`:7552`; §2.3 dropped the
    self-contradictory "world-readable" (config.json is 0600 owner-only); term drift
    (`session`/`sessionSaved`/`liveSession`) declared synonymous once (INV-1);
    `monthly[]` sort-recent-first made explicit; `since` ISODate→display reformat
    noted; new test bundle (`test_audit`) named; INV-2 test-cite corrected to TSC-2.
  Loop 3 to run cold (no fix briefing).

- **Loop 3 — two independent cold reviewers, cold (2026-07-18):** no Loop 1 or Loop 2
  finding resurfaced (all held). Tally **CRITICAL 0 · HIGH 0 · MEDIUM 3 · LOW 6 ·
  INFO 4** (all verified; both reviewers again re-confirmed every anchor). Findings
  are consistency / testability / clarity — no new architecture defect. Fixes:
  - §4 promised a null-guard inside `foldTokenSavingsIntoConfig` that is unreachable
    (the slot fires only from `m_claudeIntegration`'s own signal) → moved the guard
    to the `closeEvent` call site, where a raw deref actually could occur.
  - INV-1's `+ session` term had no falsifying test (FOLD-1 covers only stored
    arithmetic) → added TSC-8 (grep asserts `tokenSavingsSummary()` references both
    a stored getter and the live session).
  - `monthly[]` contract clarified: it holds only *folded* buckets, so the current
    month lags `month_saved` by the un-folded live session; the example values were
    made to reflect that (`12000000` bucket vs `12400000` `month_saved`).
  - Verb ordering pinned: `cmdTokenUsage` calls `tokenSavingsSummary()` before
    `ci->endTokenSession()` on `reset:true`.
  - LOW/INFO: single sort site for `monthly[]`; `(§ 2.7 helpers)` reworded (helpers
    do stored, `+ session` added in `tokenSavingsSummary`); `cachedConfig()` is
    mtime/size-guarded; HUMAN-1 gained the `1000`/`1e9` branch edges; TSC-1 keys on
    the report-valued emit; `model_state_chip` bundle attribution corrected
    (`test_core`); month/year-rollover + hover-only-cumulative notes added.
  Severity is clearly diminishing (Loop 1 schema-CRITICAL → Loop 2 quit
  double-count → Loop 3 consistency/testability refinements). Loop 4 to run cold to
  confirm a clean/polish-only convergence pass before sign-off.

- **Loop 4 — two independent cold reviewers, cold (2026-07-18): CONVERGED.** No Loop
  1/2/3 finding resurfaced. Tally **CRITICAL 0 · HIGH 0 · MEDIUM 1 · LOW 5 · INFO 3**
  — every finding polish/accuracy, none structural/mechanical/architectural. Both
  reviewers independently pronounced the spec clean ("an unusually clean spec after
  3 loops … findings are polish-only"). Polish fixes applied this pass:
  - the one MEDIUM was a self-inflicted example-number clash from Loop 3 — the
    §2.5 `monthly[2026-07]` bucket implied a 0.4M session while §2.1 shows 1.2M;
    reconciled to `11200000` (12.4M − 1.2M).
  - §2.5 reset call named precisely (`ci->resetTokenUsage()` delegating to
    `endTokenSession()`, not `ci->endTokenSession()` directly); "matching the chip"
    → "the chip's *tooltip*" (face is session-only).
  - §2.4 dropped a false justification (the connect is made alongside the existing
    `toolFinished`/`sessionStopped` connects at `mainwindow.cpp:2457`+, NOT in
    `setupStatusBarChrome`); INV-1 currency caveated (one-slot-cycle staleness
    bound); INV-3 test noted as unable to enforce thread-affinity; chip-face-blanks
    note added.
  - **Convergence (per the cold-eyes rule):** a pass whose only verified findings
    are polish IS converged — fixed the polish, stopped. The mechanism, invariants,
    layer map, and test plan are verified sound against current source; every cited
    anchor re-confirmed across four independent cold reviewers. Spec is
    implementable and ready for sign-off.
