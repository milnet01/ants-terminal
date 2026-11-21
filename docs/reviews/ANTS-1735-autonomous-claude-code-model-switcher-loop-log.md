# ANTS-1735 — Autonomous Claude Code model switcher + effectiveness ledger — review loop log

Record for [`../specs/ANTS-1735-autonomous-claude-code-model-switcher.md`](../specs/ANTS-1735-autonomous-claude-code-model-switcher.md) § Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-05-25) — 3 independent reviewers (correctness, grounding, product-fit)

Convergent CRITICAL/HIGH findings, all folded:

- **C1 (correctness)** — `currentState()` is process-global, not per-tab; the
  Idle gate could target the wrong session. → § 2.3 now reads the focused tab's
  `ClaudeTabTracker` per-shell state + `isFocusedTabSession`; new INV-2.
- **C2 / HIGH-2 / product-fit-CRITICAL** — no empty-input accessor; Claude's TUI
  composer is not a grid line; OSC 133 marks the shell, not Claude. → § 2.2
  spike S2 + § 2.4 keystroke-timing window (`composerEmpty`); INV-3 redefined,
  `lastUserKeystrokeMs` named as new work.
- **OQ-2 / product-fit-CRITICAL** — injected `/model` may raise a modal picker.
  → promoted from open question to **blocking spike S1** (§ 8).
- **HIGH-1** — `Gate.current` typed `Tier` but `score()` returns a raw string. →
  § 2.1 names `tierFromModelId(rec.currentModel)`.
- **HIGH-3** — `model_switch_stats` should be `caller_cwd` Required. → § 2.5/§4/§7
  changed to Required; INV-13.
- **H3** — override indistinguishable from controller's own injection. → § 2.5
  authorship via ledger `trigger:auto` correlation; INV-11.
- **H4** — floor-clamp + hysteresis livelock. → § 2.3 stability counted on the
  clamped target; INV-5.
- **product-fit HIGH/MEDIUM (metrics)** — override near-zero by construction
  inflates success; missed upgrades have no signal; `opus_turns_avoided` rewards
  over-aggression. → § 2.5 adds `under_route_signal` (INV-12), reports headline
  as avoided/regret ratio, acknowledges passivity confound.
- **M7 / MEDIUM-3** — pending records can be evicted before outcome fill-in. →
  § 2.5 pins pending records from eviction; INV-10.
- **MEDIUM-2** — correction regex false-fires. → § 2.5 scoped to first
  post-downgrade turn, soft signal.
- **M6** — missing security-boundary invariant. → INV-9.
- **product-fit MEDIUM** — chip re-shows during dwell. → § 2.6 suppress chip when
  auto on; INV-14.
- **OQ-3** — default-off misses the vibe-coder goal. → § 8 explicit numeric
  flip-gate + prominent toggle.
- **MEDIUM-1 (grounding)** — off line citations. → § 1/§ 2.3 corrected to
  `:147-148`, `mainwindow.cpp:749` + `:783-784`.
- **LOW-1** — Config has no generic getter. → § 2.7 typed `claudeAutoModel()`.
- **L8 / typos** — `.utf8()`→`.toUtf8()`; "confircation"→"confirmation".

Verified solid by reviewers (kept): scorer return shape, `ClaudeState` enum +
`currentState()`, the `sendToPty` injection idiom, `setOwnerOnlyPerms` +
`read_log` byte-cap idiom, config storage location/mode, the reuse-the-scorer
stance, and the goal-5 thinking-level path.

### Loop 2 (2026-05-25) — 2 fresh reviewers (correctness, grounding), no fix-briefing

Both independently converged on a HIGH the loop-1 fix introduced, plus grounding
gaps — all folded:

- **HIGH (both)** — `ClaudeState` has no `AwaitingInput` value
  (`claudeintegration.h:47-53`); `awaitingInput` is a permission-prompt bool
  overlay (`claudetabtracker.cpp:59-89`), not turn-end. Keying the injection
  window on it would make the switcher **never fire** on the normal path. →
  § 2.4 rekeyed on the transition into `ClaudeState::Idle`;
  `awaitingInputSinceMs` renamed `idleSinceMs`; INV-2/INV-3 updated.
- **HIGH (grounding)** — no "entered idle" timestamp exists; `ShellState` records
  only the current state. → § 2.4/§ 4 name `idleSinceMs` as new tracker work
  (re-stamped on every Idle entry) and specify init stamps (`0` / `-1`).
- **MEDIUM (correctness)** — `composerEmpty` init/equality hole. → init stamps
  specified; INV-3 covers the turn-end → type → next-turn sequence.
- **MEDIUM (both)** — `isFocusedTabSession` is private + the design didn't need
  the global state at all. → § 2.3 reads the focused shell's own per-tab state
  via `m_tracker->shellState(focused->shellPid())` (already used at
  `claudestatuswidgets.cpp:205-207`); `isFocusedTabSession`/`focusedIsBacking`
  dropped; INV-2 simplified.
- **MEDIUM (correctness)** — INV-11 authorship ambiguous for two auto-switches in
  one window. → correlation key added (`to_tier` + `|Δts| ≤ kAuthorWindowMs`).
- **LOW (both)** — readlog is a read-path filter, not append+evict; INV-12
  last-turn boundary. → § 2.5 reframes readlog as a *pattern* (pinning is novel);
  INV-12 adds the last-turn pending case.

Verified solid in loop 2: injection idiom (`:147-148`), scorer shape, timer
lines (`mainwindow.cpp:749`,`:783-784`), `keyPressEvent` interception
(`terminalwidget.cpp:1575`), Required-contract consistency
(`mainwindow.cpp:4048,4137`), `setOwnerOnlyPerms`+readlog drop-oldest,
`Config::roadmapStatusFilters()` getter pattern, and the OSC-133 characterisation.

**Cooperative-session-pause research (2026-05-25, user idea):** investigated
whether the Claude session could pause → let Ants switch → resume. Found
infeasible on the interactive CLI (no hook sets the model; no pause/resume; the
SDK has the capability but the CLI doesn't surface it). Folded as § 2.2 finding +
§ 5 upstream ask; the idle-window stays primary.

### Loop 3 (2026-05-25) — 2 fresh reviewers (consistency, grounding), no fix-briefing — CLEAN PASS

Both returned a clean verdict. Consistency: gate/prose/invariants agree, the
`composerEmpty` lifecycle is correct at every step, all 14 invariants testable,
standard satisfied. Grounding: every cited fact verified exact
(`claudestatuswidgets.cpp:205-207` tracker access, `terminalwidget.cpp:1575`
keyPressEvent, `secureio.h:33,37`, `readlog.cpp:126-128`,
`mainwindow.cpp:4048,4137` Required contract, scorer shape) and **no remaining
ungrounded capability** — the two new members (`idleSinceMs`,
`lastUserKeystrokeMs`) and the ledger logic are each named as new work.

Two minor items, folded inline (no loop 4):

- **MEDIUM** — INV-2 test surface named `ClaudeState` values without citing the
  enum (the membership class loop 2 caught). → INV-2 now cites
  `claudeintegration.h:47-53`; the four non-Idle values verified real.
- **LOW** — `Decision::reason` declared but unused (ledger uses the scorer's
  `rec.reason`). → dropped from the struct.

Outcome: cold-eyes converged after 3 loops. Status → awaiting user sign-off.
Implementation remains gated on spikes S1 + S2 (§ 8) and the user's
design-synthesis step; no code lands on this spec until those clear.
