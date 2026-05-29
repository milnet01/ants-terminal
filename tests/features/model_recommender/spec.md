# model_recommender — Feature Spec (ANTS-1226 Shape A)

## Purpose
Passive status-bar chip that scores session complexity from the Claude
Code transcript and recommends a model tier, saving spend on simple
turns under a Max(5) subscription.

## Active model detection
The running model is read from `message.model` of the most-recent
`assistant` turn in the transcript JSONL (verified: always present as
e.g. "claude-opus-4-7"). Returns empty string when the transcript has
no assistant turns → treated as Sonnet tier.

## Scoring algorithm (last 20 assistant turns, tail-read ≤ 512 KB)

Feature                                                                     | Weight
--------------------------------------------------------------------------- | ------
weighted_file_write_count ≥ 8 (Edit/Write × recency-weighted, see ANTS-1890)| +2 (Opus signal)
tool_diversity ≥ 6 unique tool names                                        | +1 (Opus signal)
plan_keyword in assistant text                                              | +2 (Opus: "spec","design","architecture","review","plan","refactor")
weighted_avg_message_len ≥ 500 chars                                        | +1 (Opus: long context)
file_write_count == 0 AND tool_diversity ≤ 2                                | -2 (Haiku: mechanical)

Score ≥ 2  → OPUS_TIER
Score ≤ -2 → HAIKU_TIER
Otherwise  → SONNET_TIER

**ANTS-1930 threshold rebalance.** Pre-1930 thresholds (≥ 3 Opus / ≤ -1
Haiku) were asymmetric: Opus required exceptional heavy work (max score
is 6) while Haiku triggered on a single mechanical-penalty tick. This
created a one-way downgrade ratchet — users were observed never to be
upgraded to Opus/Sonnet, only downgraded. The rebalanced thresholds
(≥ 2 Opus / ≤ -2 Haiku) restore symmetric movement: a score of +2
(e.g. a plan-keyword session with diverse tools) now upgrades, and a
score of -1 stays Sonnet rather than dropping to Haiku.

**Recency weighting (ANTS-1890).** Count-based features (writes, message
length) are weighted by `weightForTurnIndex(idx, total)` returning a
linear `w ∈ [1.0, 3.0]` so recent activity outweighs old. Set-cardinality
features (tool_diversity) and window-wide booleans (plan_keyword) stay
unweighted. The 8-threshold replaces v1's 4 (mass sum doubles from 20 to
40 across a full window).

**Commit-intent hard override (ANTS-1890).** If the latest user prompt
matches the `commit/push/stage/bump/rebase` stem regex, `score()`
returns `Tier::Haiku` with `reason="commit_intent"` directly, bypassing
the additive ladder. The autoswitcher's `clampToFloor` gate may upclamp
to the user's configured floor.

## Invariants

- **INV-1** `score(transcriptPath)` returns `SONNET_TIER` when the
  transcript file does not exist or has 0 assistant turns.
- **INV-2** Returns `OPUS_TIER` when a plan_keyword is present AND
  file_write_count ≥ 4 across the last 20 turns.
- **INV-3** Returns `HAIKU_TIER` when file_write_count == 0 AND
  tool_diversity ≤ 2 across the last 20 turns.
- **INV-4** The chip is hidden when the recommended tier matches the
  tier inferred from `message.model` in the most-recent assistant turn.
- **INV-5** The chip shows "→ Haiku" / "→ Sonnet" / "→ Opus" text when
  the recommendation differs from the current model tier.
- **INV-6** Clicking the chip calls `sendToPty("/model haiku\n")` (or
  sonnet/opus) on the focused TerminalWidget.
- **INV-7** `score()` reads at most 512 KB of trailing transcript data;
  does not load a 100 MB transcript into a QStringList.
- **INV-8** Only the last 20 assistant turns are scored (older turns
  do not influence the recommendation).
- **INV-9** `score()` is a stateless free function — no per-call
  mutable state; calling it twice with the same path and unchanged
  file returns the same result.
