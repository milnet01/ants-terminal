# Feature: Per-tab model + thinking-level status-bar chip

Part of **ANTS-1888**. Passive chip in `ClaudeStatusBarController` that
always shows the focused tab's current Claude model tier (Haiku/Sonnet/Opus)
and its last-used thinking level (standard / think / think hard /
ultrathink). Pairs with the recommender chip's INV-14 suppression — when
the auto-switcher is on the recommender hides, and this chip becomes the
visible widget in the same slot so the user is never blind to what model
is in use.

## Scope

- **Helper** — `ModelRecommender::thinkingLevelFromLatestUserTurn` +
  `thinkingLevelLabel` (pure; tail-reads ≤ 512 KB; covered by the
  `model_recommender` test bundle).
- **Chip wiring** — `m_modelStateBtn` member, `refreshModelStateChip()`
  method, status-timer connect, reset on tab switch. Read-only widget;
  click is a no-op in v1.

## Invariants

- The controller declares a `m_modelStateBtn` member sibling to
  `m_modelBtn`; it is added to the status bar via `addPermanentWidget`
  after `m_modelBtn` so when INV-14 hides the recommender, the state
  chip occupies the same visual slot.
- `refreshModelStateChip()` exists, is exposed in the public method
  list (called from the status timer), and uses the focused-terminal
  provider + `m_integration->activeSessionPath(cwd)` to resolve the
  transcript (same pattern as `refreshModelChip`).
- `mainwindow.cpp` connects the 2-second status timer's `timeout`
  signal to `ClaudeStatusBarController::refreshModelStateChip`.
- `resetForTabSwitch()` hides the chip and clears its mtime cache so
  the next tick re-reads against the newly-focused tab.
- The chip hides entirely when no transcript is found; hides only the
  thinking half (no " · …" suffix) when the level is undetectable
  (`thinkingLevelLabel(Unknown)` is empty by contract).

## Method

A source-grep contract over `src/claudestatuswidgets.{h,cpp}` and
`src/mainwindow.cpp`. The thinking-level parser itself is covered
behaviourally in the `model_recommender` test bundle (12 cases
including longest-match, slash-prefixed forms, /nothink → Standard,
substring guards, and latest-turn-wins ordering).
