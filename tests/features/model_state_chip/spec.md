# Feature: Per-tab model and thinking-level status-bar chips

Part of **ANTS-1888**. Passive chip in `ClaudeStatusBarController` that
always shows the focused tab's current Claude model, by name and version
("Fable 5.2", "Opus 5.5"; ANTS-6748). A second chip beside it shows the
thinking level Claude Code recorded for the latest turn, such as
"Thinking: medium" (ANTS-6749). The model chip pairs with the recommender
chip's INV-14 suppression — when the auto-switcher is on the recommender
hides, and this chip becomes the visible widget in the same slot so the
user is never blind to what model is in use.

## Scope

- **Helpers** — `ModelRecommender::modelDisplayName` and
  `effortFromLatestAssistantTurn` (pure; the second tail-reads the
  transcript; both covered by the `model_recommender` test bundle).
- **Chip wiring** — `m_modelStateBtn` and `m_thinkingBtn` members,
  `refreshModelStateChip()` method, status-timer connect, reset on tab
  switch. Read-only widgets.

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
- The chip's model text comes from
  `ModelRecommender::modelDisplayName(rec.currentModel)`, never from the
  recommender's `Tier`, which reads every unknown family as Sonnet
  (ANTS-6748). An id that names no family hides the chip. The parser's
  cases are in `model_recommender` (`ModelDisplayName`).
- Both chips hide when no transcript is found.
- `m_thinkingBtn` is added directly after `m_modelStateBtn`. Its text
  comes from `ModelRecommender::effortFromLatestAssistantTurn`, and it
  hides when that is empty, so it never shows an empty value. The model
  chip carries no thinking suffix (ANTS-6749).

## Method

A source-grep contract over `src/claudestatuswidgets.{h,cpp}` and
`src/mainwindow.cpp`. The two helpers are covered behaviourally in the
`model_recommender` test bundle (`ModelDisplayName`,
`ModelRecommenderEffort`).
